#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(OS_WINDOWS)
#include <windows.h>
#include <direct.h>
#define SLASHSTR "\\"

#else
#define SLASHSTR "/"
#endif


int verbose=0;

typedef struct {
#if defined(OS_WINDOWS)
	WIN32_FILE_ATTRIBUTE_DATA s;
#else
	struct stat s;
#endif
} fileinfo_t;


int parsedep(const char **ptr, char *name, int maxlen);
int get_fileinfo(const char *filename, fileinfo_t *info);
int is_dir(const fileinfo_t *info);
int is_newer(const fileinfo_t * file1, const fileinfo_t *file2);
int file_size(const fileinfo_t *info);
int create_dir(const char *name);
int files_identical(const char *file1, const char *file2, int size);
const char * unquote_arg(const char *str);
char * stringcat(const char *str1, const char *str2);
void printf_verbose(const char *format, ...) __attribute__ ((format (printf, 1, 2)));
void die(const char *format, ...) __attribute__ ((format (printf, 1, 2)));
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

void usage(void)
{
	printf("\n");
	printf("precompile_helper - Generate a precompiled header for Arduino.h\n");
	printf("\n");
	printf("Usage:\n");
	printf("  precompile_helper [-v] <srcfolder> <destfolder> <compiler args...>\n");
	printf("\n");
	exit(1);
}

int main(int argc, char **argv)
{
	const char *srcdir, *destdir, *pchdir;
	const char *destArduinofile, *srcArduinofile, *depfile, *gchfile;
	char filename[4096];
	int first_compiler_arg;
	fileinfo_t srcinfo, destinfo, pchinfo, targetinfo;
	fileinfo_t destArduinoinfo, srcArduinoinfo, depinfo, gchinfo, fileinfo;
	int i, r, size;
	char *arglist[200];
	int argcount;
	char *buffer;
	const char *p;
	FILE *fp;

	if (argc < 4) usage();
	if (strcmp(argv[1], "-v") == 0) {
		verbose = 1;
		if (argc < 5) usage();
		srcdir = unquote_arg(argv[2]);
		destdir = unquote_arg(argv[3]);
		first_compiler_arg = 4;
		printf_verbose("\n********************************************\n");
		printf_verbose("      precompile_helper " TOSTRING(VERSION) "\n");
	} else {
		srcdir = unquote_arg(argv[1]);
		destdir = unquote_arg(argv[2]);
		first_compiler_arg = 3;
	}

	// verify both srcdir and destdir exist
	if (!get_fileinfo(srcdir, &srcinfo)) die("Unable to access %s\n", srcdir);
	if (!get_fileinfo(destdir, &destinfo)) die("Unable to access %s\n", destdir);
	if (!is_dir(&srcinfo)) die("Error: %s is not a directory\n", srcdir);
	if (!is_dir(&destinfo)) die("Error: %s is not a directory\n", destdir);

	// if destdir/pch doesn't exist, create it
	pchdir = stringcat(destdir, SLASHSTR "pch");
	if (!get_fileinfo(pchdir, &pchinfo)) {
		printf_verbose("create dir: %s\n", pchdir);
		if (!create_dir(pchdir) || !get_fileinfo(pchdir, &pchinfo) || !is_dir(&pchinfo)) {
			die("Unable to create dir: %s\n", pchdir);
		}
	}

	// if destdir/pch/Arduino.h doesn't exist, or if it differs from srcdir/Arduino.h
	// copy srcdir/Arduino.h to destdir/pch/Arduino.h, and skip to compile
	destArduinofile = stringcat(pchdir, SLASHSTR "Arduino.h");
	srcArduinofile = stringcat(srcdir, SLASHSTR "Arduino.h");
	if (!get_fileinfo(srcArduinofile, &srcArduinoinfo)) {
		die("Unable to find file: %s\n", srcArduinofile);
	}
	if (!get_fileinfo(destArduinofile, &destArduinoinfo)
	  || file_size(&srcArduinoinfo) != file_size(&destArduinoinfo)
	  || !files_identical(srcArduinofile, destArduinofile, file_size(&srcArduinoinfo))) {
		size = file_size(&srcArduinoinfo);
		if (size <= 0) die("Error, file %s is empty\n", srcArduinofile);
		if (size > 65536) {
			fprintf(stderr, "Arduino.h is unexpected large\n");
			goto compile;
		}
		buffer = malloc(size);
		if (!buffer) die("Unable to allocate memory, %d bytes\n", size);
		printf_verbose("Copy %s to %s, %d bytes\n", srcArduinofile, destArduinofile, size);
		fp = fopen(srcArduinofile, "rb");
		if (!fp) die("Unable to read %s\n", srcArduinofile);
		r = fread(buffer, 1, size, fp);
		fclose(fp);
		if (r != size) die("Error reading %s\n", srcArduinofile);
		fp = fopen(destArduinofile, "wb");
		if (!fp) die("Unable to write %s\n", destArduinofile);
		r = fwrite(buffer, 1, size, fp);
		fclose(fp);
		if (r != size) die("Error writing %s\n", destArduinofile);
		free(buffer);
		goto compile;
	}

	// if destdir/pch/Arduino.h.d doesn't exist, skip to compile
	depfile = stringcat(pchdir, SLASHSTR "Arduino.h.d");
	if (!get_fileinfo(depfile, &depinfo)) goto compile;
	if (!is_newer(&depinfo, &destArduinoinfo)) {
		printf_verbose("%s is newer than %s, compile req'd\n", destArduinofile, depfile);
		goto compile;
	}
	printf_verbose("%s looks ok\n", depfile);

	// if destdir/pch/Arduino.h.gch doesn't exist, skip to compile
	gchfile = stringcat(pchdir, SLASHSTR "Arduino.h.gch");
	if (!get_fileinfo(gchfile, &gchinfo)) goto compile;
	if (!is_newer(&gchinfo, &destArduinoinfo)) {
		printf_verbose("%s is newer than %s, compile req'd\n", destArduinofile, gchfile);
		goto compile;
	}
	printf_verbose("%s looks ok\n", gchfile);

	// read destdir/pch/Arduino.h.d
	printf_verbose("Read %s...\n", depfile);
	size = file_size(&depinfo);
	if (size <= 0) {
		fprintf(stderr, "File %s is empty\n", depfile);
		goto compile;
	}
	if (size > 262144) {
		fprintf(stderr, "%s is unexpected large, not parsing\n", depfile);
		goto compile;
	}
	buffer = malloc(size + 1);
	if (!buffer) {
		fprintf(stderr, "Unable to allocate memory, %d bytes\n", size);
		goto compile;
	}
	fp = fopen(depfile, "rb");
	if (!fp) {
		fprintf(stderr, "Unable to read %s\n", depfile);
		goto compile;
	}
	r = fread(buffer, 1, size, fp);
	buffer[size] = 0;
	fclose(fp);
	if (r != size) {
		fprintf(stderr, "Error reading %s\n", depfile);
		goto compile;
	}

	// parse dependency list, any file newer than destdir/pch/Arduino.h.gch, compile
	printf_verbose("Parse depfile, %d bytes\n", size);
	p = buffer;
	if (!parsedep(&p, filename, sizeof(filename))) goto compile;
	r = strlen(filename);
	if (r < 1 || filename[r-1] != ':') {
		printf_verbose("target file doesn't end with colon: %s\n", filename);
		goto compile;
	}
	filename[r-1] = 0; // delete trailing colon
	printf_verbose("target file: %s\n", filename);
	if (!get_fileinfo(filename, &targetinfo)) {
		printf_verbose("  can't get info for this file\n");
		goto compile;
	}
	while (parsedep(&p, filename, sizeof(filename))) {
		printf_verbose(" dep file: %s\n", filename);
		if (!get_fileinfo(filename, &fileinfo)) {
			printf_verbose("  can't get info for this file\n");
			goto compile;
		}
		if (!is_newer(&targetinfo, &fileinfo)) {
			printf_verbose("  newer than target, compile req'd\n");
			goto compile;
		}
		if (!is_newer(&gchinfo, &fileinfo)) {
			printf_verbose("  newer than %s, compile req'd\n", gchfile);
			goto compile;
		}
		if (!is_newer(&depinfo, &fileinfo)) {
			printf_verbose("  newer than %s, compile req'd\n", depfile);
			goto compile;
		}
	}

	// ok, done, destdir/pch/Arduino.h.gch can be used as-is :)
	printf_verbose("All dependency checks passed, no need to run compiler\n");
	printf("Using previously compiled file: %s\n", gchfile);
	printf_verbose("\n********************************************\n");
	return 0;

compile:
	printf_verbose("Running Compiler:\n");
	if (argc - first_compiler_arg + 2 > sizeof(arglist)/sizeof(char *)) {
		die("Error: too many compiler args!\n");
	}
	for (i = first_compiler_arg; i < argc; i++) {
		arglist[i - first_compiler_arg] = argv[i];
		printf_verbose("arg: %s\n", argv[i]);
	}
	argcount = argc - first_compiler_arg;
	printf_verbose("prog: %s\n", argv[first_compiler_arg]);
	printf_verbose("\n********************************************\n");
	fflush(stdout);
	fflush(stderr);
#if defined(OS_WINDOWS)
	char line[32768];
	int len;
	len = snprintf(line, sizeof(line), "\"%s\"", arglist[0]);
	for (i = 1; i < argcount; i++) {
		if (strchr(arglist[i], ' ')) {
			len += snprintf(line + len, sizeof(line) - len, " \"%s\"", arglist[i]);
		} else {
			len += snprintf(line + len, sizeof(line) - len, " %s", arglist[i]);
		}
	}
	for (i = 0; i < len; i++) {
		if (line[i] == '/') line[i] = '\\';
	}
	printf_verbose("command line: %s\n", line);
	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;
	HANDLE h_rd=NULL, h_wr=NULL;
	if (!CreatePipe(&h_rd, &h_wr, &sa, 0)) {
		die("precompile_helper: CreatePipe fail\n");
	}
	if (!SetHandleInformation(h_rd, HANDLE_FLAG_INHERIT, 0)) {
		die("precompile_helper: SetHandleInformation fail\n");
	}
	PROCESS_INFORMATION pi;
	STARTUPINFO si;
	memset(&pi, 0, sizeof(PROCESS_INFORMATION));
	memset(&si, 0, sizeof(STARTUPINFO));
	si.hStdError = h_wr;
	si.hStdOutput = h_wr;
	si.hStdInput = INVALID_HANDLE_VALUE;
	si.dwFlags = STARTF_USESTDHANDLES;
	printf_verbose("CreateProcess attempt\n");
	fflush(stdout);
	if (!CreateProcess(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
		die("precompile_helper: CreateProcess fail\n");
	}
	CloseHandle(h_wr);
	printf_verbose("CreateProcess success\n");
	HANDLE hlist[2];
	hlist[0] = h_rd;
	hlist[1] = pi.hProcess;
	// listen for any compiler messages while we wait for it to finish
	while (1) {
		DWORD n = WaitForMultipleObjects(2, hlist, FALSE, INFINITE);
		if (n == WAIT_OBJECT_0) {
			// compiler printed something to stdout or stderr
			char cbuf[16384];
			DWORD len=0;
			BOOL r = ReadFile(h_rd, cbuf, sizeof(cbuf), &len, NULL);
			if (!r || len == 0) break;
			char *src = cbuf;
			char *dst = cbuf;
			for (i=0; i < len; i++) {   // delete any '\r' chars, which
				if (*src != '\r') { // cause double space in Arduino
					*dst++ = *src++;
				} else {
					src++;
				}
			}
			len = dst - cbuf;
			if (len > 0) {
				fwrite(cbuf, 1, len, stdout);
				fflush(stdout);
			}
		} else if (n == WAIT_OBJECT_0 + 1) {
			// compiled completed
			break;
		}
	}
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
#else
	arglist[argcount] = NULL;
	execv(argv[first_compiler_arg], arglist);
#endif
	return 0;
}


// extract the next filename from the compiler's .d dependency list
int parsedep(const char **ptr, char *name, int maxlen)
{
	const char *p = *ptr;
	int count = 0;

	// skip leading while space
	while (1) {
		if (*p == '\\' && (*(p+1) == '\r' || *(p+1) == '\n')) {
			p += 2;
		} else if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
			p++;
		} else if (*p == 0) {
			name[0] = 0;
			*ptr = p; // end of data
			return 0;
		} else {
			break;
		}
	}
	// copy filename
	while (count < maxlen - 1) {
		if (*p == ' ' || *p == '\r' || *p == '\n' || *p == 0) {
			break;
		}
		if (*p == '\\' && *(p+1) == ' ') {
			name[count++] = ' ';
			p++;
		} else {
			name[count++] = *p;
		}
		p++;
	}
	name[count] = 0;
	*ptr = p;
	if (count == 0) return 0;
	return 1;
}


int get_fileinfo(const char *filename, fileinfo_t *info)
{
	if (!filename || !info) return 0;
#if defined(OS_WINDOWS)
	if (!(GetFileAttributesEx(filename, GetFileExInfoStandard, &(info->s)))) return 0;
#else
	if (stat(filename, &(info->s)) != 0) return 0;
#endif
	return 1;
}

int is_dir(const fileinfo_t *info)
{
	if (!info) return 0;
#if defined(OS_WINDOWS)
	if (!(info->s.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
	return 1;
#else
	return S_ISDIR(info->s.st_mode);
#endif
}

int file_size(const fileinfo_t *info)
{
	if (!info) return 0;
#if defined(OS_WINDOWS)
	return info->s.nFileSizeLow;
#else
	return info->s.st_size;
#endif
}

// return 1 if file1 is newer than file2, or 0 if they are equal or file1 is older
int is_newer(const fileinfo_t *file1, const fileinfo_t *file2)
{
#if defined(OS_WINDOWS)
	if (file1->s.ftLastWriteTime.dwHighDateTime > file2->s.ftLastWriteTime.dwHighDateTime ||
	  (file1->s.ftLastWriteTime.dwHighDateTime == file2->s.ftLastWriteTime.dwHighDateTime &&
	  file1->s.ftLastWriteTime.dwLowDateTime > file2->s.ftLastWriteTime.dwLowDateTime)) {
		return 1;
	}
#elif defined(OS_MACOSX)
	if (file1->s.st_mtimespec.tv_sec > file2->s.st_mtimespec.tv_sec ||
	  (file1->s.st_mtimespec.tv_sec == file2->s.st_mtimespec.tv_sec &&
	  file1->s.st_mtimespec.tv_nsec > file2->s.st_mtimespec.tv_nsec)) {
		return 1;
	}
#elif defined(OS_LINUX32) || defined(OS_LINUX64) || defined(OS_LINUXARM)
	if (file1->s.st_mtime > file2->s.st_mtime ||
	  (file1->s.st_mtime == file2->s.st_mtime &&
	  file1->s.st_mtim.tv_nsec > file2->s.st_mtim.tv_nsec)) {
		return 1;
	}
#endif
	return 0;
}



int create_dir(const char *name)
{
#if defined(OS_WINDOWS)
	if (_mkdir(name) == 0) return 1;
#else
	if (mkdir(name, 0777) == 0) return 1;
#endif
	return 0;
}

int files_identical(const char *file1, const char *file2, int size)
{
	char *buf1=NULL, *buf2=NULL;
	FILE *fp;
	int r, ret=0;

	buf1 = malloc(size);
	if (!buf1) goto end;
	buf2 = malloc(size);
	if (!buf2) goto end;
	fp = fopen(file1, "rb");
	if (!fp) goto end;
	r = fread(buf1, 1, size, fp);
	fclose(fp);
	if (r != size) goto end;
	fp = fopen(file2, "rb");
	if (!fp) goto end;
	r = fread(buf2, 1, size, fp);
	fclose(fp);
	if (r != size) goto end;
	if (memcmp(buf1, buf2, size) == 0) {
		printf_verbose("identical files: %s and %s\n", file1, file2);
		ret = 1;
	}
end:
	if (buf1) free(buf1);
	if (buf2) free(buf2);
	return ret;
}


const char * unquote_arg(const char *str)
{
	int len;

	if (str == NULL) return NULL;
	len = strlen(str);
	if (len > 0 && str[0] == '"') {
		if (len > 2 && str[len-1] == '"') {
			char *newstr = strdup(str+1);
			if (newstr == NULL) {
				fprintf(stderr, "precompile_helper: unable to allocate memory\n");
				fflush(stderr);
				return NULL;
			}
			newstr[len-2] = 0;
			return newstr;
		}
	}
	return str;
}

char * stringcat(const char *str1, const char *str2)
{
	static char badout=0;
	int len1 = strlen(str1);
	int len2 = strlen(str2);
	char *out = (char *)malloc(len1 + len2 + 1);
	if (!out) {
		fprintf(stderr, "precompile_helper: unable to allocate memory\n");
		fflush(stderr);
		return &badout;
	}
	strcpy(out, str1);
	strcpy(out + len1, str2);
	return out;
}

void printf_verbose(const char *format, ...)
{
	va_list args;
	if (verbose) {
		va_start(args, format);
		vprintf(format, args);
		va_end(args);
	}
}

void die(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stdout, format, args);
	va_end(args);
	fflush(stdout);
	exit(1);
}
