/*
 * mpxgen - FM multiplex encoder with Stereo and RDS
 * Copyright (C) 2019 Anthony96922
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common.h"
#include "ascii_cmd.h"
#include "control_pipe.h"

#ifdef _WIN32

/* Windows named pipe implementation */
static HANDLE hPipe = INVALID_HANDLE_VALUE;
static OVERLAPPED olap;
static HANDLE hEvent = NULL;

int open_control_pipe(char *filename) {
	char pipe_name[256];

	/* Convert a simple name into a Windows named pipe path */
	if (filename[0] != '\\') {
		snprintf(pipe_name, sizeof(pipe_name), "\\\\.\\pipe\\%s", filename);
	} else {
		snprintf(pipe_name, sizeof(pipe_name), "%s", filename);
	}

	hPipe = CreateNamedPipeA(
		pipe_name,
		PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
		1,		/* max instances */
		0,		/* out buffer size */
		CTL_BUFFER_SIZE,/* in buffer size */
		0,		/* default timeout */
		NULL		/* security attributes */
	);

	if (hPipe == INVALID_HANDLE_VALUE) return -1;

	memset(&olap, 0, sizeof(olap));
	hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	olap.hEvent = hEvent;

	/* Start waiting for a client connection (non-blocking) */
	ConnectNamedPipe(hPipe, &olap);

	return 0;
}

void poll_control_pipe() {
	static unsigned char pipe_buf[CTL_BUFFER_SIZE];
	static unsigned char cmd_buf[CMD_BUFFER_SIZE];
	DWORD bytes_read = 0;
	DWORD result;
	char *token;

	/* Check if there's data available (with timeout) */
	result = WaitForSingleObject(hEvent, READ_TIMEOUT_MS);
	if (result != WAIT_OBJECT_0) return;

	ResetEvent(hEvent);

	memset(pipe_buf, 0, CTL_BUFFER_SIZE);

	if (!ReadFile(hPipe, pipe_buf, CTL_BUFFER_SIZE - 1, &bytes_read, &olap)) {
		DWORD err = GetLastError();
		if (err == ERROR_IO_PENDING) {
			GetOverlappedResult(hPipe, &olap, &bytes_read, TRUE);
		} else if (err == ERROR_BROKEN_PIPE) {
			/* Client disconnected - reconnect */
			DisconnectNamedPipe(hPipe);
			ResetEvent(hEvent);
			ConnectNamedPipe(hPipe, &olap);
			return;
		} else {
			return;
		}
	}

	if (bytes_read == 0) {
		/* Client disconnected */
		DisconnectNamedPipe(hPipe);
		ResetEvent(hEvent);
		ConnectNamedPipe(hPipe, &olap);
		return;
	}

	/* handle commands per line */
	token = strtok((char *)pipe_buf, "\n");
	while (token != NULL) {
		memset(cmd_buf, 0, CMD_BUFFER_SIZE);
		memcpy(cmd_buf, token, CMD_BUFFER_SIZE - 1);
		token = strtok(NULL, "\n");

		process_ascii_cmd(cmd_buf);
	}
}

void close_control_pipe() {
	if (hPipe != INVALID_HANDLE_VALUE) {
		DisconnectNamedPipe(hPipe);
		CloseHandle(hPipe);
		hPipe = INVALID_HANDLE_VALUE;
	}
	if (hEvent != NULL) {
		CloseHandle(hEvent);
		hEvent = NULL;
	}
}

#else /* POSIX */

static int fd;
static struct pollfd poller;

/*
 * Opens a file (pipe) to be used to control the RDS coder.
 */
int open_control_pipe(char *filename) {
	fd = open(filename, O_RDONLY | O_NONBLOCK);
	if (fd == -1) return -1;

	/* setup the poller */
	poller.fd = fd;
	poller.events = POLLIN;

	return 0;
}

/*
 * Polls the control file (pipe), and if a command is received,
 * calls process_ascii_cmd.
 */
void poll_control_pipe() {
	static unsigned char pipe_buf[CTL_BUFFER_SIZE];
	static unsigned char cmd_buf[CMD_BUFFER_SIZE];
	struct timeval timeout;
	int ret;
	fd_set set;
	char *token;

	FD_ZERO(&set);
	FD_SET(fd, &set);
	timeout.tv_sec = 0;
	timeout.tv_usec = READ_TIMEOUT_MS * 1000;

	/* check for new commands */
	if (poll(&poller, 1, READ_TIMEOUT_MS) <= 0) return;

	/* return early if there are no new commands */
	if (poller.revents == 0) return;

	memset(pipe_buf, 0, CTL_BUFFER_SIZE);

	ret = select(fd + 1, &set, NULL, NULL, &timeout);
	if (ret == -1 || ret == 0) {
		return;
	} else {
		read(fd, pipe_buf, CTL_BUFFER_SIZE - 1);
	}

	/* handle commands per line */
	token = strtok((char *)pipe_buf, "\n");
	while (token != NULL) {
		memset(cmd_buf, 0, CMD_BUFFER_SIZE);
		memcpy(cmd_buf, token, CMD_BUFFER_SIZE - 1);
		token = strtok(NULL, "\n");

		process_ascii_cmd(cmd_buf);
	}
}

void close_control_pipe() {
	if (fd > 0) close(fd);
}

#endif /* _WIN32 */
