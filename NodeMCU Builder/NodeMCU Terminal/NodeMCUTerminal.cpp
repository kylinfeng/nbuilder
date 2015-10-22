// NodeMCU Terminal.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"


//#define max(a, b) (a > b ? a : b)
//#define min(a, b) (a < b ? a : b)

typedef BOOL (*PROCESS_LINE_ROUTINE)(char*);
typedef BOOL (*READ_RESULT_ROUTINE)(char*, int, BOOL);

READ_RESULT_ROUTINE pfOnReadResult = NULL;

CRITICAL_SECTION csCommandLock = {};

HANDLE hCom = NULL;
HANDLE hReadThread = NULL;
HANDLE hWriteThread = NULL;
HANDLE hCanWriteEvent = NULL;
HANDLE hOnTaskEvent = NULL;
HANDLE hOnWriteEvent = NULL; // Ensure current command all write finish
HANDLE hOnReplyEvent = NULL; // Ensure current command read reply finish

// IPC Command
HANDLE hIpcCommandBuffer = NULL;
HANDLE hIpcReceivedEvent = NULL;
HANDLE hIpcReadyEvent = NULL;
HANDLE hIpcThread = NULL;
LPVOID pIpcCommandBuffer = NULL;

typedef struct _IpcCommand {
	DWORD dwCmdType;
	WCHAR cmdParam1[1224];
	WCHAR cmdParam2[1224];
} IpcCommand;

char readEnd[8] = {0};

WCHAR downFilePath[MAX_PATH];
HANDLE hDownScript = NULL;

WCHAR pullFolderPath[MAX_PATH];
WCHAR pullFilePath[MAX_PATH];
HANDLE hPullFile = NULL;

#define DO_COMMAND 1
#define DO_EXECUTE 2
#define DO_DOWNLOAD 4
#define DO_PULL 8
#define DO_STAY 0x80000000

int doWhat = 0;
char doCommand[4096];
WCHAR doFilePath[MAX_PATH];
char doPullName[520];

inline PWSTR GetIpcReceivedEventName(PWSTR buffer, PCWSTR comName)
{
	return lstrcatW(lstrcpyW(buffer, L"NODEMCU_TERMINAL_IPC_COMMAND_RECEIVED_ON_"), comName);
}

inline PWSTR GetIpcReadyEventName(PWSTR buffer, PCWSTR comName)
{
	return lstrcatW(lstrcpyW(buffer, L"NODEMCU_TERMINAL_IPC_THREAD_READY_ON_"), comName);
}

inline PWSTR GetIpcBufferName(PWSTR buffer, PCWSTR comName)
{
	return lstrcatW(lstrcpyW(buffer, L"NODEMCU_TERMINAL_IPC_COMMAND_BUFFER_ON_"), comName);
}

void WaitReply()
{
	WaitForSingleObject(hOnWriteEvent, INFINITE);
	WaitForSingleObject(hOnReplyEvent, INFINITE);
}

inline void CommandLock()
{
	EnterCriticalSection(&csCommandLock);
}

inline void CommandUnlock()
{
	LeaveCriticalSection(&csCommandLock);
}

BOOL PrintResult(char* readResult, int length, BOOL readOver)
{
	if(*readResult != 0)
	{
		printf("%s", readResult);
	}
	return TRUE;
}

void CorrectPullFile(void)
{
	const char boundarySign[] = "!<lUaBuFfErBoUnDaRy>!\r\n";
	const int boundarySignLength = sizeof(boundarySign) - 1;
	DWORD dwFileSize = GetFileSize(hPullFile, NULL), dwNewSize = dwFileSize;
	HANDLE hMapping = CreateFileMapping(hPullFile, NULL, PAGE_READWRITE, 0, 0, NULL);
	if(hMapping != NULL)
	{
		LPVOID pView = MapViewOfFile(hMapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
		if(pView != NULL)
		{
			int boundaryCount = 0;
			char* pData = (char*)pView;
			for(int offset = 0; offset < (int)dwFileSize - boundarySignLength * boundaryCount; ++offset)
			{
				if(!memcmp(pData + offset, boundarySign, boundarySignLength))
				{
					++boundaryCount;
					if(offset < (int)dwFileSize - boundarySignLength * boundaryCount)
					{
						memcpy(pData + offset, pData + offset + boundarySignLength, (int)dwFileSize - offset - boundarySignLength * boundaryCount);
					}
				}
			}
			dwNewSize = dwFileSize - boundarySignLength * boundaryCount;
			char* pAppend = pData + (int)dwNewSize - 4;
			if(!memcmp(pAppend, "\r\n> ", 4))
			{
				dwNewSize -= 4;
			}
			UnmapViewOfFile(pView);
		}
		CloseHandle(hMapping);
	}
	if(dwNewSize != dwFileSize)
	{
		SetFilePointer(hPullFile, dwNewSize, NULL, FILE_BEGIN);
		SetEndOfFile(hPullFile);
	}
}

BOOL DoPullResult(char* readResult, int length, BOOL readOver)
{
	BOOL retVal = FALSE;
	if(length > 0)
	{
		DWORD dwWrite = 0;
		retVal = WriteFile(hPullFile, readResult, length, &dwWrite, NULL);
		wprintf(L".");
	}
	if(readOver)
	{
		CorrectPullFile();
		pfOnReadResult = PrintResult;
		CloseHandle(hPullFile);
		wprintf(L"\r\nfinished.\r\n> ");
	}
	return retVal;
}

DWORD WINAPI ReadThread(LPVOID lparam)
{
	COMSTAT comStat = {};
	DWORD dwErrorFlags = CE_RXOVER | CE_OVERRUN | CE_RXPARITY | CE_FRAME | CE_BREAK | CE_TXFULL | CE_IOE | CE_MODE;
	if(ClearCommBreak(hCom) && ClearCommError(hCom, &dwErrorFlags, &comStat))
	{
		int cmdLength = 0, cmdOffset = 0; // Used when remove command echo
		char buffer[4096] = {0};
		DWORD dwRead = 0;
		while(ReadFile(hCom, buffer, sizeof(buffer) - 1, &dwRead, NULL))
		{
			if(dwRead == 0)
			{
				Sleep(1);
			}
			else
			{
				buffer[dwRead] = 0;
				if(dwRead >= 4)
				{
					lstrcpyA(readEnd, &buffer[dwRead - 4]);
				}
				else
				{
					int length = lstrlenA(readEnd);
					if(length > (int)(4 - dwRead))
					{
						char remain[8] = {0};
						lstrcpyA(remain, &readEnd[length - (4 - dwRead)]);
						lstrcpyA(readEnd, remain);
					}
					lstrcatA(readEnd, buffer);
				}
				BOOL readOver = !lstrcmpA(readEnd, "\r\n> ") || !lstrcmpA(readEnd, "\n>> ");
				if(readOver)
				{
					doCommand[0] = 0;
					cmdLength = 0, cmdOffset = 0;
				}
				if(pfOnReadResult != NULL)
				{
					char* pResult = buffer;
					if(cmdLength == 0 && lstrlenA(doCommand) > 0)
					{
						lstrcatA(doCommand, "\r\n"); // To remove the append "\r\n"
						cmdLength = lstrlenA(doCommand);
					}
					if(cmdLength != cmdOffset) // Remove command echo here
					{
						int cmpLength = min((int)dwRead, cmdLength - cmdOffset);
						int idx = 0;
						for(; idx < cmpLength; ++idx)
						{
							if(buffer[idx] != doCommand[cmdOffset + idx])
							{
								doCommand[0] = 0;
								break;
							}
						}
						cmdOffset += idx;
						pResult += idx;
					}
					pfOnReadResult(pResult, (int)dwRead - (pResult - buffer), readOver);
				}
				if(readOver)
				{
					SetEvent(hCanWriteEvent);
					SetEvent(hOnReplyEvent);
				}
			}
		}
	}
	return 0;
}

BOOL DoCommand(char* pCommand)
{
	BOOL retVal = FALSE;
	if(WAIT_OBJECT_0 == WaitForSingleObject(hCanWriteEvent, INFINITE))
	{
		COMSTAT comStat = {};
		DWORD dwErrorFlags = CE_RXOVER | CE_OVERRUN | CE_RXPARITY | CE_FRAME | CE_BREAK | CE_TXFULL | CE_IOE | CE_MODE;
		if(ClearCommBreak(hCom) && ClearCommError(hCom, &dwErrorFlags, &comStat))
		{
			char command[4096] = {};
			if(lstrlenA(pCommand) > sizeof(command) - 1 - 1)
			{
				return FALSE;
			}
			lstrcpyA(command, pCommand);
			lstrcatA(command, "\r");
			DWORD dwWrite = 0;
			if(WriteFile(hCom, command, lstrlenA(command), &dwWrite, NULL))
			{
				ResetEvent(hOnReplyEvent);
				retVal = TRUE;
			}
		}
	}
	return retVal;
}

BOOL ProcessFile(PCWSTR lpFilePath, PROCESS_LINE_ROUTINE doLine)
{
	BOOL retVal = FALSE;
	HANDLE hFile = CreateFile(lpFilePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if(hFile != INVALID_HANDLE_VALUE)
	{
		HANDLE hMapping = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
		if(hMapping != NULL)
		{
			LPVOID pView = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
			if(pView != NULL)
			{
				retVal = TRUE;
				char* pData = (char*)pView;
				DWORD dwSize = GetFileSize(hFile, NULL);
				DWORD dwBegin = 0, dwEnd = 0;
				while(true)
				{
					dwBegin = dwEnd;
					while(dwBegin < dwSize && (pData[dwBegin] == '\r' || pData[dwBegin] == '\n'))
					{
						++dwBegin;
					}
					if(dwBegin == dwSize)
					{
						break;
					}
					dwEnd = dwBegin;
					while(dwEnd < dwSize && pData[dwEnd] != '\r' && pData[dwEnd] != '\n')
					{
						++dwEnd;
					}
					char lineBuf[4096] = {0};
					if(dwEnd - dwBegin > sizeof(lineBuf))
					{
						return FALSE;
					}
					for(int idx = 0; idx < (int)(dwEnd - dwBegin); ++idx)
					{
						lineBuf[idx] = pData[dwBegin + idx];
					}
					if(doLine != NULL)
					{
						doLine(lineBuf);
					}
				}
				UnmapViewOfFile(pView);
			}
			CloseHandle(hMapping);
		}
		CloseHandle(hFile);
	}
	else
	{
		wprintf(L"Error: %u when open file: %s.\r\n> ", GetLastError(), lpFilePath);
	}
	return retVal;
}

DWORD WINAPI WriteThread(LPVOID lparam)
{
	while(WaitForSingleObject(hOnTaskEvent, INFINITE) == WAIT_OBJECT_0)
	{
		CommandLock();
		switch(doWhat)
		{
		case DO_COMMAND:
		case DO_PULL:
			{
				DoCommand(doCommand);
			}
			break;
		case DO_EXECUTE:
		case DO_DOWNLOAD:
			{
				ProcessFile(doFilePath, DoCommand);
			}
		default:
			break;
		}
		doWhat = 0;
		CommandUnlock();
		SetEvent(hOnWriteEvent);
	}
	return 0;
}

BOOL InitComm(PCWSTR comName)
{
	hCom = CreateFile(comName,
       GENERIC_READ | GENERIC_WRITE,
       0,
       NULL,
       OPEN_EXISTING,
       0,
       NULL);

	if(hCom != INVALID_HANDLE_VALUE)
	{
		DCB dcb = {};
		dcb.DCBlength = sizeof(DCB);
		if(GetCommState(hCom, &dcb))
		{
			dcb.BaudRate = CBR_9600;
			dcb.ByteSize = 8;
			dcb.Parity = NOPARITY;
			dcb.StopBits = ONESTOPBIT;
			COMMTIMEOUTS timeOuts = {0};
			if(GetCommTimeouts(hCom, &timeOuts))
			{
				timeOuts.ReadIntervalTimeout = MAXDWORD;
				timeOuts.ReadTotalTimeoutConstant = 0;
				timeOuts.ReadTotalTimeoutMultiplier = MAXDWORD;
				timeOuts.WriteTotalTimeoutConstant = 0;
				timeOuts.WriteTotalTimeoutMultiplier = MAXDWORD;
				if(SetCommState(hCom, &dcb))
				{
					if(SetCommTimeouts(hCom, &timeOuts))
					{
						if(SetupComm(hCom, 65536, 65536))
						{
							if(PurgeComm(hCom, PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR))
							{
								COMSTAT comStat = {};
								DWORD dwErrorFlags = CE_RXOVER | CE_OVERRUN | CE_RXPARITY | CE_FRAME | CE_BREAK | CE_TXFULL | CE_IOE | CE_MODE;
								if(ClearCommBreak(hCom) && ClearCommError(hCom, &dwErrorFlags, &comStat))
								{
									return TRUE;
								}
							}
						}
					}
				}
			}
		}
		CloseHandle(hCom);
	}
	return FALSE;
}

BOOL InitDown(void)
{
	WCHAR tempPath[MAX_PATH];
	if(GetTempPath(MAX_PATH, tempPath))
	{
		if(GetTempFileName(tempPath, L"ntds_", 0, downFilePath))
		{
			hDownScript = CreateFile(downFilePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			if(hDownScript != INVALID_HANDLE_VALUE)
			{
				return TRUE;
			}
			else
			{
				wprintf(L"Error: %u when open file: %s.\r\n> ", GetLastError(), downFilePath);
			}
		}
	}
	return FALSE;
}

BOOL DoDownLine(char* lineData)
{
	char lineBuf[4096];
	char prefix[] = "file.writeline(\"";
	char append[] = "\")\r\n";
	int length = lstrlenA(lineData);
	if(length > 4096 - 1 - sizeof(prefix) + 1 - sizeof(append) + 1)
	{
		return FALSE;
	}
	lstrcpyA(lineBuf, prefix);
	char* curChar = lineBuf + sizeof(prefix) - 1;
	for(int idx = 0; idx < length && curChar - lineBuf < sizeof(lineBuf); ++idx)
	{
		if(lineData[idx] == '\\' || lineData[idx] == '\"')
		{
			*curChar++ = '\\';
			if(curChar - lineBuf < sizeof(lineBuf))
			{
				*curChar++ = lineData[idx];
			}
		}
		else
		{
			*curChar++ = lineData[idx];
		}
	}
	if(curChar - lineBuf >= sizeof(lineBuf))
	{
		return FALSE;
	}
	*curChar = 0;
	lstrcatA(lineBuf, "\")\r\n");
	DWORD dwWrite = 0;
	return WriteFile(hDownScript, lineBuf, lstrlenA(lineBuf), &dwWrite, NULL);
}

BOOL InitPull(PCWSTR fileName, PCWSTR folderPath)
{
	if(folderPath != NULL)
	{
		lstrcpyW(pullFilePath, folderPath);
	}
	else
	{
		GetCurrentDirectory(MAX_PATH, pullFilePath);
	}
	if(lstrlenW(pullFilePath) > 0)
	{
		int pathLength = lstrlenW(pullFilePath);
		if(pullFilePath[pathLength - 1] != L'\\')
		{
			pullFilePath[pathLength] = L'\\';
			pullFilePath[pathLength + 1] = 0;
		}
		lstrcatW(pullFilePath, fileName);
		hPullFile = CreateFile(pullFilePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if(hPullFile != INVALID_HANDLE_VALUE)
		{
			return TRUE;
		}
		else
		{
			wprintf(L"Error: %u when open file: %s.\r\n> ", GetLastError(), fileName);
		}
	}
	return FALSE;
}

void OnCommand(char* inCommand, BOOL echo, char* cheat)
{
	WaitReply();
	CommandLock();
	if(echo)
	{
		printf("%s\r\n", inCommand);
	}
	else if(cheat != NULL)
	{
		printf("%s\r\n", cheat);
	}
	doWhat = DO_COMMAND;
	lstrcpyA(doCommand, inCommand);
	SetEvent(hOnTaskEvent);
	CommandUnlock();
}

void OnExecute(char* inPath, BOOL echo)
{
	WCHAR filePath[MAX_PATH] = {0};
	MultiByteToWideChar(CP_ACP, 0, inPath, -1, filePath, MAX_PATH);
	HANDLE hFile = CreateFile(filePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if(hFile != INVALID_HANDLE_VALUE)
	{
		CloseHandle(hFile);
		WaitReply();
		CommandLock();
		if(echo)
		{
			printf(".exec %s\r\n", inPath);
		}
		doWhat = DO_EXECUTE;
		lstrcpyW(doFilePath, filePath);
		SetEvent(hOnTaskEvent);
		CommandUnlock();
	}
	else
	{
		printf("Error: %u when open file: %s.\r\n> ", GetLastError(), inPath);
	}
}

void OnDownload(char* inPath, BOOL echo)
{
	WCHAR filePath[MAX_PATH] = {0};
	MultiByteToWideChar(CP_ACP, 0, inPath, -1, filePath, MAX_PATH);
	HANDLE hFile = CreateFile(filePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if(hFile != INVALID_HANDLE_VALUE)
	{
		CloseHandle(hFile);
		if(InitDown())
		{
			char* fileName = &inPath[lstrlenA(inPath) - 1];
			while(fileName - inPath > 0 && *(fileName - 1) != '\\')
			{
				--fileName;
			}
			DWORD dwWrite = 0;
			char startScript[4096];
			lstrcpyA(startScript, "file.open(\"");
			lstrcatA(startScript, fileName);
			lstrcatA(startScript, "\", \"w\")\r\n");
			WriteFile(hDownScript, startScript, lstrlenA(startScript), &dwWrite, NULL);
			BOOL bScriptOk = ProcessFile(filePath, DoDownLine);
			char endScript[] = "file.close()\r\n";
			WriteFile(hDownScript, endScript, lstrlenA(endScript), &dwWrite, NULL);
			CloseHandle(hDownScript);
			if(bScriptOk)
			{
				WaitReply();
				CommandLock();
				if(echo)
				{
					printf(".down %s\r\n", inPath);
				}
				doWhat = DO_DOWNLOAD;
				lstrcpyW(doFilePath, downFilePath);
				SetEvent(hOnTaskEvent);
				CommandUnlock();
			}
			return;
		}
	}
	else
	{
		printf("Error: %u when open file: %s.\r\n> ", GetLastError(), inPath);
	}
}

void OnPull(char* inFileName, char* folderPath, BOOL echo)
{
	WCHAR fileName[MAX_PATH], dirPath[MAX_PATH];
	MultiByteToWideChar(CP_ACP, 0, inFileName, -1, fileName, MAX_PATH);
	if(folderPath != NULL)
	{
		MultiByteToWideChar(CP_ACP, 0, folderPath, -1, dirPath, MAX_PATH);
	}
	if(InitPull(fileName, folderPath != NULL ? dirPath : NULL))
	{
		WaitReply();
		CommandLock();
		if(echo)
		{
			printf(".pull %s\r\n", inFileName);
		}
		printf("Pulling file %s:\r\n.", inFileName);
		pfOnReadResult = DoPullResult;
		doWhat = DO_PULL;
		lstrcpyA(doCommand, "file.open(\"");
		lstrcatA(doCommand, inFileName);
		lstrcatA(doCommand, "\",\"r\");d=file.read();while d~=nil do print(d..\"!<lUaBuFfErBoUnDaRy>!\");d=file.read();end;file.close()");
		SetEvent(hOnTaskEvent);
		CommandUnlock();
	}
}

DWORD WINAPI IpcWaitThread(LPVOID lparam)
{
	char cmdParam1[2048], cmdParam2[2048];
	while(WAIT_OBJECT_0 == WaitForSingleObject(hIpcReceivedEvent, INFINITE))
	{
		IpcCommand* ipcCmd = (IpcCommand*)pIpcCommandBuffer;
		WideCharToMultiByte(CP_ACP, 0, ipcCmd->cmdParam1, -1, cmdParam1, sizeof(cmdParam1), NULL, NULL);
		if(ipcCmd->dwCmdType & DO_COMMAND)
		{
			OnCommand(cmdParam1, FALSE, ".list");
		}
		else if(ipcCmd->dwCmdType & DO_EXECUTE)
		{
			OnExecute(cmdParam1, TRUE);
		}
		else if(ipcCmd->dwCmdType & DO_DOWNLOAD)
		{
			OnDownload(cmdParam1, TRUE);
		}
		else if(ipcCmd->dwCmdType & DO_PULL)
		{
			WideCharToMultiByte(CP_ACP, 0, ipcCmd->cmdParam2, -1, cmdParam2, sizeof(cmdParam2), NULL, NULL);
			OnPull(cmdParam1, cmdParam2, TRUE);
		}
		SetEvent(hIpcReadyEvent);
	}
	return 0;
}

int InteractiveLoop()
{
	wchar_t* helpText = L"************************************************************\r\n"
		L"*                                                          *\r\n"
		L"*    To do a command on your nodemcu, just do it without   *\r\n"
		L"*    the prefix dot.                                       *\r\n"
		L"*                                                          *\r\n"
		L"*    To execute a lua file on your computer:               *\r\n"
		L"*        .exec C:\\somepath\\somefile.lua                    *\r\n"
		L"*                                                          *\r\n"
		L"*    To download a lua file to your nodemcu:               *\r\n"
		L"*        .down C:\\somepath\\somefile.lua                    *\r\n"
		L"*                                                          *\r\n"
		L"*    To pull a lua file to your computer:                  *\r\n"
		L"*        .pull somefile.lua                                *\r\n"
		L"*                                                          *\r\n"
		L"*    To list the lua files on your nodemcu:                *\r\n"
		L"*        .list                                             *\r\n"
		L"*                                                          *\r\n"
		L"*    To quit the interactive loop:                         *\r\n"
		L"*        .quit                                             *\r\n"
		L"*                                                          *\r\n"
		L"************************************************************\r\n";
	wprintf(L"%s", helpText);
	char userCmd[4096] = {0};
	DWORD dwRead = 0;
	HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
	while(ReadFile(hStdIn, userCmd, sizeof(userCmd), &dwRead, NULL))
	{
		userCmd[dwRead] = 0;
		char* tailCmd = &userCmd[dwRead - 1];
		while(tailCmd - userCmd >= 0 && (*tailCmd == '\r' || *tailCmd == '\n'))
		{
			*tailCmd-- = 0;
		}
		if(userCmd[0] != '.')
		{
			OnCommand(userCmd, FALSE, NULL);
		}
		else
		{
			char* headCmd = userCmd;
			while(headCmd - userCmd < (int)dwRead)
			{
				if(*headCmd == ' ')
				{
					*headCmd++ = 0;
					break;
				}
				++headCmd;
			}
			while(headCmd - userCmd < (int)dwRead && *headCmd == ' ')
			{
				++headCmd;
			}
			if(!lstrcmpiA(userCmd, ".exec"))
			{
				OnExecute(headCmd, FALSE);
			}
			else if(!lstrcmpiA(userCmd, ".down"))
			{
				OnDownload(headCmd, FALSE);
			}
			else if(!lstrcmpiA(userCmd, ".pull"))
			{
				OnPull(headCmd, NULL, FALSE);
			}
			else if(!lstrcmpiA(userCmd, ".quit"))
			{
				break;
			}
			else if(!lstrcmpiA(userCmd, ".list"))
			{
				OnCommand("for k,v in pairs(file.list()) do print(\"name: \"..k..\"  \tsize: \"..v);end", FALSE, NULL);
			}
			else
			{
				printf("Error command: %s\r\n> ", userCmd);
			}
		}
	}
	return 0;
}

BOOL InitIpc(PCWSTR comName)
{
	WCHAR objectName[4096];
	hIpcReceivedEvent = CreateEvent(NULL, FALSE, FALSE, GetIpcReceivedEventName(objectName, comName));
	if(hIpcReceivedEvent == NULL)
	{
		return FALSE;
	}
	hIpcReadyEvent = CreateEvent(NULL, FALSE, TRUE, GetIpcReadyEventName(objectName, comName));
	if(hIpcReadyEvent == NULL)
	{
		return FALSE;
	}
	hIpcCommandBuffer = CreateFileMapping(NULL, NULL, PAGE_READWRITE, 0, 4096, GetIpcBufferName(objectName, comName));
	if(hIpcCommandBuffer == NULL)
	{
		return FALSE;
	}
	pIpcCommandBuffer = MapViewOfFile(hIpcCommandBuffer, FILE_MAP_READ, 0, 0, 4096);
	if(pIpcCommandBuffer == NULL)
	{
		return FALSE;
	}
	DWORD ipcThreadId = 0;
	hIpcThread = CreateThread(NULL, 0, &IpcWaitThread, NULL, 0, &ipcThreadId);
	if(hIpcThread == NULL)
	{
		return FALSE;
	}
	return TRUE;
}

// -com:COM3
// -exec:path
// -down:path
// -pull:name
// -list
BOOL ParseCmdLine(int argc, PWSTR argv[], PWSTR comName, PDWORD cmdType, PWSTR cmdParam)
{
	BOOL retVal = FALSE;
	if(argc < 2)
	{
		return FALSE;
	}
	for(int idx = 1; idx < argc; ++idx)
	{
		int cmdLength = lstrlenW(argv[idx]);
		if(cmdLength < 5)
		{
			wprintf(L"Error param :%s\r\n", argv[idx]);
			continue;
		}
		WCHAR prefix[8] = {0};
		for(int cur = 0; cur < 6; ++cur)
		{
			prefix[cur] = argv[idx][cur];
		}
		if(!lstrcmpiW(prefix, L"-exec:"))
		{
			*cmdType |= DO_EXECUTE;
			lstrcpyW(cmdParam, argv[idx] + 6);
		}
		else if(!lstrcmpiW(prefix, L"-down:"))
		{
			*cmdType |= DO_DOWNLOAD;
			lstrcpyW(cmdParam, argv[idx] + 6);
		}
		else if(!lstrcmpiW(prefix, L"-pull:"))
		{
			*cmdType |= DO_PULL;
			lstrcpyW(cmdParam, argv[idx] + 6);
		}
		else if(prefix[5] = 0, !lstrcmpiW(prefix, L"-list"))
		{
			*cmdType |= DO_COMMAND;
			lstrcpyW(cmdParam, L"for k,v in pairs(file.list()) do print(\"name: \"..k..\", size: \"..v);end");
		}
		else if(prefix[5] = 0, !lstrcmpiW(prefix, L"-stay"))
		{
			*cmdType |= DO_STAY;
		}
		else if(prefix[5] = 0, !lstrcmpiW(prefix, L"-com:"))
		{
			lstrcpyW(comName, argv[idx] + 5);
			retVal = TRUE;
		}
	}
	return retVal;
}

int _tmain(int argc, _TCHAR* argv[])
{
	WCHAR comName[4096];
	WCHAR cmdParam[4096];
	DWORD dwCmdType = 0;
	if(ParseCmdLine(argc, argv, comName, &dwCmdType, cmdParam))
	{
		if(InitComm(comName))
		{
			InitializeCriticalSection(&csCommandLock);
			pfOnReadResult = PrintResult;
			hCanWriteEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
			hOnTaskEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
			hOnWriteEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
			hOnReplyEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
			DWORD readThreadId = 0;
			hReadThread = CreateThread(NULL, 0, &ReadThread, NULL, 0, &readThreadId);
			DWORD writeThreadId = 0;
			hWriteThread = CreateThread(NULL, 0, &WriteThread, NULL, 0, &writeThreadId);
			char commandParam[4096];
			WideCharToMultiByte(CP_ACP, 0, cmdParam, -1, commandParam, 4096, NULL, NULL);
			BOOL bInteractive = FALSE;
			if(dwCmdType & DO_STAY)
			{
				bInteractive = TRUE;
			}
			if(dwCmdType == 0)
			{
				bInteractive = TRUE;
			}
			else if(dwCmdType & DO_COMMAND)
			{
				OnCommand(commandParam, FALSE, NULL);
			}
			else if(dwCmdType & DO_EXECUTE)
			{
				OnExecute(commandParam, FALSE);
			}
			else if(dwCmdType & DO_DOWNLOAD)
			{
				OnDownload(commandParam, FALSE);
			}
			else if(dwCmdType & DO_PULL)
			{
				OnPull(commandParam, NULL, FALSE);
			}
			if(!bInteractive)
			{
				WaitReply();
				//WaitForSingleObject(hWriteThread, INFINITE);
				//WaitForSingleObject(hReadThread, INFINITE);
			}
			else
			{
				if(InitIpc(comName))
				{
					InteractiveLoop();
				}
			}
			if(bInteractive)
			{
				TerminateThread(hIpcThread, 0);
				UnmapViewOfFile(pIpcCommandBuffer);
				CloseHandle(hIpcReceivedEvent);
				CloseHandle(hIpcReadyEvent);
				CloseHandle(hIpcThread);
				CloseHandle(hIpcCommandBuffer);
			}
			TerminateThread(hWriteThread, 0);
			TerminateThread(hReadThread, 0);
			CloseHandle(hOnTaskEvent);
			CloseHandle(hReadThread);
			CloseHandle(hWriteThread);
			CloseHandle(hCom);
			DeleteCriticalSection(&csCommandLock);
		}
		else
		{
			if((dwCmdType & DO_COMMAND) || (dwCmdType & DO_EXECUTE) || (dwCmdType & DO_DOWNLOAD) || (dwCmdType & DO_PULL))
			{
				WCHAR objectName[4096];
				hIpcReadyEvent = OpenEvent(SYNCHRONIZE, FALSE, GetIpcReadyEventName(objectName, comName));
				hIpcReceivedEvent = OpenEvent(EVENT_MODIFY_STATE, FALSE, GetIpcReceivedEventName(objectName, comName));
				hIpcCommandBuffer = OpenFileMapping(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, GetIpcBufferName(objectName, comName));
				pIpcCommandBuffer = MapViewOfFile(hIpcCommandBuffer, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 4096);
				if(hIpcReadyEvent != NULL && hIpcReceivedEvent != NULL && hIpcCommandBuffer != NULL && pIpcCommandBuffer != NULL)
				{
					if(WAIT_OBJECT_0 == WaitForSingleObject(hIpcReadyEvent, INFINITE))
					{
						IpcCommand* ipcCmd = (IpcCommand*)pIpcCommandBuffer;
						ipcCmd->dwCmdType = dwCmdType;
						lstrcpyW(ipcCmd->cmdParam1, cmdParam);
						if(dwCmdType & DO_PULL)
						{
							GetCurrentDirectory(MAX_PATH, ipcCmd->cmdParam2);
						}
						SetEvent(hIpcReceivedEvent);
					}
					UnmapViewOfFile(pIpcCommandBuffer);
					CloseHandle(hIpcCommandBuffer);
					CloseHandle(hIpcReceivedEvent);
					CloseHandle(hIpcReadyEvent);
				}
				else
				{
					wprintf(L"IPC open object error.\r\n");
				}
			}
			else
			{
				wprintf(L"Open %s error.\r\n", comName);
			}
		}
	}
	else
	{
		wchar_t* helpText = L"************************************************************\r\n"
			L"*                                                          *\r\n"
			L"*    To execute a lua file on your computer:               *\r\n"
			L"*        nterm -com:COM3 -exec:C:\\somepath\\somefile.lua    *\r\n"
			L"*                                                          *\r\n"
			L"*    To download a lua file to your nodemcu:               *\r\n"
			L"*        nterm -com:COM3 -down:C:\\somepath\\somefile.lua    *\r\n"
			L"*                                                          *\r\n"
			L"*    To pull a lua file to your computer:                  *\r\n"
			L"*         nterm -com:COM3 -pull:somefile.lua               *\r\n"
			L"*                                                          *\r\n"
			L"*    To list the lua files on your nodemcu:                *\r\n"
			L"*         nterm -com:COM3 -list                            *\r\n"
			L"*                                                          *\r\n"
			L"*    You can only specify the COM Port to enter intera-    *\r\n"
			L"*    ctive mode.                                           *\r\n"
			L"*                                                          *\r\n"
			L"************************************************************\r\n";
		wprintf(L"%s", helpText);
	}
	return 0;
}

// EOF
