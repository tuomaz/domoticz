/*
 Domoticz, Open Source Home Automation System

 Copyright (C) 2012,2015 Rob Peters (GizMoCuz)

 Domoticz is free software: you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published
 by the Free Software Foundation, either version 3 of the License,
 or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty
 of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 See the GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with Domoticz. If not, see <http://www.gnu.org/licenses/>.
*/

#include "stdafx.h"
#include "mainworker.h"
#include <stdio.h>
#include <sys/types.h>
#include <signal.h>
#include <iostream>
#include "CmdLine.h"
#include "Logger.h"
#include "Helper.h"
#include "WebServerHelper.h"
#include "SQLHelper.h"
#include "../notifications/NotificationHelper.h"
#include "appversion.h"
#include "localtime_r.h"

#if defined WIN32
	#include "../msbuild/WindowsHelper.h"
	#include <Shlobj.h>
#else
	#include <sys/stat.h>
	#include <unistd.h>
	#include <syslog.h>
	#include <errno.h>
	#include <fcntl.h>
	#include <string.h> 
#endif

#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
static void dumpstack(void) {
	// Notes :
	// The following code does needs -rdynamic compile option.not print full backtrace.
	// To have a full backtrace you need to :
	// - compile with -g -rdynamic options
	// - active core dump using "ulimit -c unlimited" before starting daemon
	// - use gdb to analyze the core dump
	void *addrs[128];
	int n, count = backtrace(addrs, 128);
	char** symbols = backtrace_symbols(addrs, count);

	if (symbols) {
		for (n = 0; n < count; n++) {
			_log.Log(LOG_ERROR, "  %s", symbols[n]);
		}
		free(symbols);
	}
}
#else
static void dumpstack(void) {
}
#endif

const char *szHelp=
	"Usage: Domoticz -www port -verbose x\n"
	"\t-www port (for example -www 8080, or -www 0 to disable http)\n"
	"\t-wwwbind address (for example -wwwbind 0.0.0.0 or -wwwbind 192.168.0.20)\n"
#ifdef WWW_ENABLE_SSL
	"\t-sslwww port (for example -sslwww 443, or -sslwww 0 to disable https)\n"
	"\t-sslcert file_path (for example /opt/domoticz/server_cert.pem)\n"
	"\t-sslpass passphrase (to access to server private key in certificate)\n"
	"\t-sslmethod method (for SSL method)\n"
	"\t-ssloptions options (for SSL options, default is 'default_workarounds,no_sslv2,single_dh_use')\n"
	"\t-ssldhparam file_path (for SSL DH parameters)\n"
#endif
#if defined WIN32
	"\t-wwwroot file_path (for example D:\\www)\n"
	"\t-dbase file_path (for example D:\\domoticz.db)\n"
	"\t-userdata file_path (for example D:\\domoticzdata)\n"
#else
	"\t-wwwroot file_path (for example /opt/domoticz/www)\n"
	"\t-dbase file_path (for example /opt/domoticz/domoticz.db)\n"
	"\t-userdata file_path (for example /opt/domoticz)\n"
#endif
	"\t-webroot additional web root, useful with proxy servers (for example domoticz)\n"
	"\t-verbose x (where x=0 is none, x=1 is all important, x=2 is debug)\n"
	"\t-startupdelay seconds (default=0)\n"
	"\t-nowwwpwd (in case you forgot the web server username/password)\n"
	"\t-nocache (do not return appcache, use only when developing the web pages)\n"
#if defined WIN32
	"\t-nobrowser (do not start web browser (Windows Only)\n"
#endif
#if defined WIN32
	"\t-log file_path (for example D:\\domoticz.log)\n"
#else
	"\t-log file_path (for example /var/log/domoticz.log)\n"
#endif
	"\t-loglevel (0=All, 1=Status+Error, 2=Error)\n"
	"\t-notimestamps (do not prepend timestamps to logs; useful with syslog, etc.)\n"
	"\t-php_cgi_path (for example /usr/bin/php-cgi)\n"
#ifndef WIN32
	"\t-daemon (run as background daemon)\n"
	"\t-pidfile pid file location (for example /var/run/domoticz.pid)\n"
	"\t-syslog [user|daemon|local0 .. local7] (use syslog as log output, defaults to facility 'user')\n"
#endif
	"";

#ifndef WIN32
struct _facilities {
	const char* facname;
	const int   facvalue;
};

static const _facilities facilities[] =
{
	{ "daemon", LOG_DAEMON },
	{ "user",   LOG_USER },
	{ "local0", LOG_LOCAL0 },
	{ "local1", LOG_LOCAL1 },
	{ "local2", LOG_LOCAL2 },
	{ "local3", LOG_LOCAL3 },
	{ "local4", LOG_LOCAL4 },
	{ "local5", LOG_LOCAL5 },
	{ "local6", LOG_LOCAL6 },
	{ "local7", LOG_LOCAL7 }
}; 
std::string logfacname = "user";
#endif
std::string szStartupFolder;
std::string szUserDataFolder;
std::string szWWWFolder;
std::string szWebRoot;

bool bHasInternalTemperature=false;
std::string szInternalTemperatureCommand = "/opt/vc/bin/vcgencmd measure_temp";

bool bHasInternalVoltage=false;
std::string szInternalVoltageCommand = "";

bool bHasInternalCurrent=false;
std::string szInternalCurrentCommand = "";


std::string szAppVersion="???";
std::string szAppHash="???";
std::string szAppDate="???";
int ActYear;
time_t m_StartTime=time(NULL);

MainWorker m_mainworker;
CLogger _log;
http::server::CWebServerHelper m_webservers;
CSQLHelper m_sql;
CNotificationHelper m_notifications;
std::string logfile = "";
bool g_bStopApplication = false;
bool g_bUseSyslog = false;
bool g_bRunAsDaemon = false;
bool g_bDontCacheWWW = false;
int pidFilehandle = 0;

#define DAEMON_NAME "domoticz"
#define PID_FILE "/var/run/domoticz.pid" 

int fatal_handling = 0;

void signal_handler(int sig_num)
{
	switch(sig_num)
	{
#ifndef WIN32
	case SIGHUP:
		if (logfile!="")
		{
			_log.SetOutputFile(logfile.c_str());
		}
		break;
#endif
	case SIGINT:
	case SIGTERM:
#ifndef WIN32
		if ((g_bRunAsDaemon)||(g_bUseSyslog))
			syslog(LOG_INFO, "Domoticz is exiting...");
#endif
		g_bStopApplication = true;
		break;
	case SIGSEGV:
	case SIGILL:
	case SIGABRT:
	case SIGFPE:
		if (fatal_handling) {
			_log.Log(LOG_ERROR, "Domoticz received fatal signal %d while backtracing !...", sig_num);
			exit(EXIT_FAILURE);
		}
		fatal_handling = 1;
		_log.Log(LOG_ERROR, "Domoticz received fatal signal %d !...", sig_num);
		dumpstack();
		// re-raise signal to enforce core dump
		signal(sig_num, SIG_DFL);
		raise(sig_num);
		break;
	} 
}

#ifndef WIN32
void daemonShutdown()
{
	if (pidFilehandle != 0) {
		close(pidFilehandle);
		pidFilehandle = 0;
	}
}

void daemonize(const char *rundir, const char *pidfile)
{
	int pid, sid, i;
	char str[10];
	struct sigaction newSigAction;
	sigset_t newSigSet;

	/* Check if parent process id is set */
	if (getppid() == 1)
	{
		/* PPID exists, therefore we are already a daemon */
		return;
	}

	/* Set signal mask - signals we want to block */
	sigemptyset(&newSigSet);
	sigaddset(&newSigSet, SIGCHLD);  /* ignore child - i.e. we don't need to wait for it */
	sigaddset(&newSigSet, SIGTSTP);  /* ignore Tty stop signals */
	sigaddset(&newSigSet, SIGTTOU);  /* ignore Tty background writes */
	sigaddset(&newSigSet, SIGTTIN);  /* ignore Tty background reads */
	sigprocmask(SIG_BLOCK, &newSigSet, NULL);   /* Block the above specified signals */

	/* Set up a signal handler */
	newSigAction.sa_handler = signal_handler;
	sigemptyset(&newSigAction.sa_mask);
	newSigAction.sa_flags = 0;

	/* Signals to handle */
	sigaction(SIGTERM, &newSigAction, NULL);    // catch term signal
	sigaction(SIGINT,  &newSigAction, NULL);    // catch interrupt signal
	sigaction(SIGSEGV, &newSigAction, NULL);    // catch segmentation fault signal
	sigaction(SIGABRT, &newSigAction, NULL);    // catch abnormal termination signal
	sigaction(SIGILL,  &newSigAction, NULL);    // catch invalid program image
#ifndef WIN32
	sigaction(SIGHUP,  &newSigAction, NULL);    // catch HUP, for logrotation
#endif
	
	/* Fork*/
	pid = fork();

	if (pid < 0)
	{
		/* Could not fork */
		exit(EXIT_FAILURE);
	}

	if (pid > 0)
	{
		/* Child created ok, so exit parent process */
		exit(EXIT_SUCCESS);
	}

	/* Ensure only one copy */
	pidFilehandle = open(pidfile, O_RDWR | O_CREAT, 0600);

	if (pidFilehandle == -1)
	{
		/* Couldn't open lock file */
		syslog(LOG_INFO, "Could not open PID lock file %s, exiting", pidfile);
		exit(EXIT_FAILURE);
	}

	/* Try to lock file */
	if (lockf(pidFilehandle, F_TLOCK, 0) == -1)
	{
		/* Couldn't get lock on lock file */
		syslog(LOG_INFO, "Could not lock PID lock file %s, exiting", pidfile);
		exit(EXIT_FAILURE);
	}

	/* Get and format PID */
	sprintf(str, "%d\n", getpid());

	/* write pid to lockfile */
	int twrite=write(pidFilehandle, str, strlen(str));
	if (twrite != strlen(str))
	{
		syslog(LOG_INFO, "Could not write to lockfile %s, exiting", pidfile);
		exit(EXIT_FAILURE);
	}


	/* Child continues */

	umask(027); /* Set file permissions 750 */

	if (logfile!="")
	{
		_log.SetOutputFile(logfile.c_str());
	}

	/* Get a new process group */
	sid = setsid();

	if (sid < 0)
	{
		exit(EXIT_FAILURE);
	}

	/* Close out the standard file descriptors */
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	/* Route I/O connections */

	/* Open STDIN */
	i = open("/dev/null", O_RDWR);

	/* STDOUT */
	int dret = dup(i);
	if (dret == -1)
	{
		_log.Log(LOG_ERROR, "Could not set STDOUT descriptor !");
	}

	/* STDERR */
	dret = dup(i);
	if (dret == -1)
	{
		_log.Log(LOG_ERROR, "Could not set STDERR descriptor !");
	}

	int cdret = chdir(rundir); /* change running directory */
	if (cdret == -1)
	{
		_log.Log(LOG_ERROR, "Could not change running directory !");
	}
}
#endif

#if defined(_WIN32)
	static size_t getExecutablePathName(char* pathName, size_t pathNameCapacity)
	{
		return GetModuleFileNameA(NULL, pathName, (DWORD)pathNameCapacity);
	}
#elif defined(__linux__) || defined(__CYGWIN32__) /* elif of: #if defined(_WIN32) */
	#include <unistd.h>
	static size_t getExecutablePathName(char* pathName, size_t pathNameCapacity)
	{
		size_t pathNameSize = readlink("/proc/self/exe", pathName, pathNameCapacity - 1);
		pathName[pathNameSize] = '\0';
		return pathNameSize;
	}
#elif defined(__FreeBSD__)
	#include <sys/sysctl.h>
	static size_t getExecutablePathName(char* pathName, size_t pathNameCapacity)
	{
		int mib[4];
		mib[0] = CTL_KERN;
		mib[1] = KERN_PROC;
		mib[2] = KERN_PROC_PATHNAME;
		mib[3] = -1;
		size_t cb = pathNameCapacity-1;
		sysctl(mib, 4, pathName, &cb, NULL, 0);
		return cb;
	}
#elif defined(__APPLE__) /* elif of: #elif defined(__linux__) */
	#include <mach-o/dyld.h>
	static size_t getExecutablePathName(char* pathName, size_t pathNameCapacity)
	{
		uint32_t pathNameSize = 0;

		_NSGetExecutablePath(NULL, &pathNameSize);

		if (pathNameSize > pathNameCapacity)
			pathNameSize = pathNameCapacity;

		if (!_NSGetExecutablePath(pathName, &pathNameSize))
		{
			char real[PATH_MAX];

			if (realpath(pathName, real) != NULL)
			{
				pathNameSize = strlen(real);
				strncpy(pathName, real, pathNameSize);
			}

			return pathNameSize;
		}

		return 0;
	}
#else /* else of: #elif defined(__APPLE__) */
	#error provide your own getExecutablePathName implementation
#endif /* end of: #if defined(_WIN32) */

void GetAppVersion()
{
	std::stringstream sstr;
	sstr << VERSION_STRING << APPVERSION;
	szAppVersion = sstr.str();
	szAppHash = APPHASH;
	char szTmp[200];
	struct tm ltime;
	time_t btime = APPDATE;
	localtime_r(&btime, &ltime);
	ActYear = ltime.tm_year + 1900;
	strftime(szTmp, 200, "%Y-%m-%d %H:%M:%S", &ltime);
	szAppDate = szTmp;
}

#if defined WIN32
int WINAPI WinMain(_In_ HINSTANCE hInstance,_In_opt_ HINSTANCE hPrevInstance,_In_ LPSTR lpCmdLine,_In_ int nShowCmd)
#else
int main(int argc, char**argv)
#endif
{
#if defined WIN32
#ifndef _DEBUG
	CreateMutexA(0, FALSE, "Local\\Domoticz"); 
	if(GetLastError() == ERROR_ALREADY_EXISTS) { 
		MessageBox(HWND_DESKTOP,"Another instance of Domoticz is already running!","Domoticz",MB_OK);
		return 1; 
	}
#endif //_DEBUG
	bool bStartWebBrowser = true;
	RedirectIOToConsole();
#endif //WIN32

	szStartupFolder = "";
	szWWWFolder = "";
	szWebRoot = "";
	
	CCmdLine cmdLine;

	// parse argc,argv 
#if defined WIN32
	cmdLine.SplitLine(__argc, __argv);
#else
	cmdLine.SplitLine(argc, argv);
	//ignore pipe errors
	signal(SIGPIPE, SIG_IGN);
#endif

	if (cmdLine.HasSwitch("-log"))
	{
		if (cmdLine.GetArgumentCount("-log") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify an output log file");
			return 1;
		}
		logfile = cmdLine.GetSafeArgument("-log", 0, "domoticz.log");
		_log.SetOutputFile(logfile.c_str());
	}
	if (cmdLine.HasSwitch("-loglevel"))
	{
		if (cmdLine.GetArgumentCount("-loglevel") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify logfile output level (0=All, 1=Status+Error, 2=Error)");
			return 1;
		}
		int Level = atoi(cmdLine.GetSafeArgument("-loglevel", 0, "").c_str());
		_log.SetVerboseLevel((_eLogFileVerboseLevel)Level);
	}
	if (cmdLine.HasSwitch("-notimestamps"))
	{
		_log.EnableLogTimestamps(false);
	}

	if (cmdLine.HasSwitch("-approot"))
	{
		if (cmdLine.GetArgumentCount("-approot") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify a APP root path");
			return 1;
		}
		std::string szroot = cmdLine.GetSafeArgument("-approot", 0, "");
		if (szroot.size() != 0)
			szStartupFolder = szroot;
	}

	if (szStartupFolder == "")
	{
#if !defined WIN32
		char szStartupPath[255];
		getExecutablePathName((char*)&szStartupPath,255);
		szStartupFolder=szStartupPath;
		if (szStartupFolder.find_last_of('/')!=std::string::npos)
			szStartupFolder=szStartupFolder.substr(0,szStartupFolder.find_last_of('/')+1);
#else
#ifndef _DEBUG
		char szStartupPath[255];
		char * p;
		GetModuleFileName(NULL, szStartupPath, sizeof(szStartupPath));
		p = szStartupPath + strlen(szStartupPath);

		while (p >= szStartupPath && *p != '\\')
			p--;

		if (++p >= szStartupPath)
			*p = 0;
		szStartupFolder=szStartupPath;
		size_t start_pos = szStartupFolder.find("\\Release\\");
		if(start_pos != std::string::npos) {
			szStartupFolder.replace(start_pos, 9, "\\domoticz\\");
			_log.Log(LOG_STATUS,"%s",szStartupFolder.c_str());
		}
#endif
#endif
	}
	GetAppVersion();
	_log.Log(LOG_STATUS, "Domoticz V%s (c)2012-%d GizMoCuz", szAppVersion.c_str(), ActYear);
	_log.Log(LOG_STATUS, "Build Hash: %s, Date: %s", szAppHash.c_str(), szAppDate.c_str());

#if !defined WIN32
	//Check if we are running on a RaspberryPi
	std::string sLine = "";
	std::ifstream infile;

#if defined(__FreeBSD__)
	infile.open("/compat/linux/proc/cpuinfo");
#else
	infile.open("/proc/cpuinfo");
#endif
	if (infile.is_open())
	{
		while (!infile.eof())
		{
			getline(infile, sLine);
			if (
				(sLine.find("BCM2708")!=std::string::npos)||
				(sLine.find("BCM2709")!=std::string::npos)
				)
			{
				//Core temperature of BCM2835 SoC
				_log.Log(LOG_STATUS,"System: Raspberry Pi");
				szInternalTemperatureCommand="/opt/vc/bin/vcgencmd measure_temp";
				bHasInternalTemperature=true;
				break;
			}
		}
		infile.close();
	}
	if (!bHasInternalTemperature)
	{
		if (file_exist("/sys/devices/platform/sunxi-i2c.0/i2c-0/0-0034/temp1_input"))
		{
			_log.Log(LOG_STATUS,"System: Cubieboard/Cubietruck");
			szInternalTemperatureCommand="cat /sys/devices/platform/sunxi-i2c.0/i2c-0/0-0034/temp1_input | awk '{ printf (\"temp=%0.2f\\n\",$1/1000); }'";
			bHasInternalTemperature = true;
		}
		else if (file_exist("/sys/devices/virtual/thermal/thermal_zone0/temp"))
		{
			//_log.Log(LOG_STATUS,"System: ODroid");
			szInternalTemperatureCommand="cat /sys/devices/virtual/thermal/thermal_zone0/temp | awk '{ if ($1 < 100) printf(\"temp=%d\\n\",$1); else printf (\"temp=%0.2f\\n\",$1/1000); }'";
			bHasInternalTemperature = true;
		}
	}
	if (file_exist("/sys/class/power_supply/ac/voltage_now"))
	{
		szInternalVoltageCommand = "cat /sys/class/power_supply/ac/voltage_now | awk '{ printf (\"volt=%0.2f\\n\",$1/1000000); }'";
		bHasInternalVoltage = true;
	}
	if (file_exist("/sys/class/power_supply/ac/current_now"))
	{
		szInternalCurrentCommand = "cat /sys/class/power_supply/ac/current_now | awk '{ printf (\"curr=%0.2f\\n\",$1/1000000); }'";
		bHasInternalCurrent = true;
	}
	_log.Log(LOG_STATUS,"Startup Path: %s", szStartupFolder.c_str());
#endif

	szWWWFolder = szStartupFolder + "www";

	if ((cmdLine.HasSwitch("-h")) || (cmdLine.HasSwitch("--help")) || (cmdLine.HasSwitch("/?")))
	{
		_log.Log(LOG_NORM, szHelp);
		return 0;
	}

	szUserDataFolder=szStartupFolder;
	if (cmdLine.HasSwitch("-userdata"))
	{
		if (cmdLine.GetArgumentCount("-userdata") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify a path for user data to be stored");
			return 1;
		}
		std::string szroot = cmdLine.GetSafeArgument("-userdata", 0, "");
		if (szroot.size() != 0)
			szUserDataFolder = szroot;
	}

	if (cmdLine.HasSwitch("-startupdelay"))
	{
		if (cmdLine.GetArgumentCount("-startupdelay") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify a startupdelay");
			return 1;
		}
		int DelaySeconds = atoi(cmdLine.GetSafeArgument("-startupdelay", 0, "").c_str());
		_log.Log(LOG_STATUS, "Startup delay... waiting %d seconds...", DelaySeconds);
		sleep_seconds(DelaySeconds);
	}

	http::server::server_settings webserver_settings;
	if (cmdLine.HasSwitch("-wwwbind"))
	{
		if (cmdLine.GetArgumentCount("-wwwbind") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify an address");
			return 1;
		}
		webserver_settings.listening_address = cmdLine.GetSafeArgument("-wwwbind", 0, "0.0.0.0");
	}

	if (cmdLine.HasSwitch("-www"))
	{
		if (cmdLine.GetArgumentCount("-www") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify a port");
			return 1;
		}
		std::string wwwport = cmdLine.GetSafeArgument("-www", 0, "");
		webserver_settings.listening_port = wwwport;
	}

	if (cmdLine.HasSwitch("-php_cgi_path"))
	{
		if (cmdLine.GetArgumentCount("-php_cgi_path") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify the path to the php-cgi command");
			return 1;
		}
		webserver_settings.php_cgi_path = cmdLine.GetSafeArgument("-php_cgi_path", 0, "");
	}
	if (cmdLine.HasSwitch("-wwwroot"))
	{
		if (cmdLine.GetArgumentCount("-wwwroot") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify a WWW root path");
			return 1;
		}
		std::string szroot = cmdLine.GetSafeArgument("-wwwroot", 0, "");
		if (szroot.size() != 0)
			szWWWFolder = szroot;
	}
	webserver_settings.www_root = szWWWFolder;
	m_mainworker.SetWebserverSettings(webserver_settings);
#ifdef WWW_ENABLE_SSL
	http::server::ssl_server_settings secure_webserver_settings;
	if (cmdLine.HasSwitch("-sslwww"))
	{
		if (cmdLine.GetArgumentCount("-sslwww") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify a port");
			return 1;
		}
		std::string wwwport = cmdLine.GetSafeArgument("-sslwww", 0, "");
		secure_webserver_settings.listening_port = wwwport;
	}
	if (!webserver_settings.listening_address.empty()) {
		// Secure listening address has to be equal
		secure_webserver_settings.listening_address = webserver_settings.listening_address;
	}
	if (cmdLine.HasSwitch("-sslcert"))
	{
		if (cmdLine.GetArgumentCount("-sslcert") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify a file path for your server certificate file");
			return 1;
		}
		secure_webserver_settings.cert_file_path = cmdLine.GetSafeArgument("-sslcert", 0, "");
	}
	if (cmdLine.HasSwitch("-sslpass"))
	{
		if (cmdLine.GetArgumentCount("-sslpass") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify a passphrase to access to your server private key in certificate file");
			return 1;
		}
		secure_webserver_settings.private_key_pass_phrase = cmdLine.GetSafeArgument("-sslpass", 0, "");
	}
	if (cmdLine.HasSwitch("-sslmethod"))
	{
		if (cmdLine.GetArgumentCount("-sslmethod") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify a SSL method");
			return 1;
		}
		secure_webserver_settings.ssl_method = cmdLine.GetSafeArgument("-sslmethod", 0, "");
	}
	if (cmdLine.HasSwitch("-ssloptions"))
	{
		if (cmdLine.GetArgumentCount("-ssloptions") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify SSL options");
			return 1;
		}
		secure_webserver_settings.options = cmdLine.GetSafeArgument("-ssloptions", 0, "");
	}
	if (cmdLine.HasSwitch("-ssldhparam"))
	{
		if (cmdLine.GetArgumentCount("-ssldhparam") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify a file path for the SSL DH parameters file");
			return 1;
		}
		secure_webserver_settings.tmp_dh_file_path = cmdLine.GetSafeArgument("-ssldhparam", 0, "");
	}
	if (cmdLine.HasSwitch("-php_cgi_path"))
	{
		if (cmdLine.GetArgumentCount("-php_cgi_path") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify the path to the php-cgi command");
			return 1;
		}
		secure_webserver_settings.php_cgi_path = cmdLine.GetSafeArgument("-php_cgi_path", 0, "");
	}
	secure_webserver_settings.www_root = szWWWFolder;
	m_mainworker.SetSecureWebserverSettings(secure_webserver_settings);
#endif
	if (cmdLine.HasSwitch("-nowwwpwd"))
	{
		m_mainworker.m_bIgnoreUsernamePassword = true;
	}
	if (cmdLine.HasSwitch("-nocache"))
	{
		g_bDontCacheWWW = true;
	}
	std::string dbasefile = szUserDataFolder + "domoticz.db";
#ifdef WIN32
#ifndef _DEBUG
	if (!IsUserAnAdmin())
	{
		char szPath[MAX_PATH];
		HRESULT hr = SHGetFolderPath(NULL, CSIDL_COMMON_APPDATA, NULL, 0, szPath);
		if (SUCCEEDED(hr))
		{
			std::string sPath = szPath;
			sPath += "\\Domoticz";

			DWORD dwAttr = GetFileAttributes(sPath.c_str());
			BOOL bDirExists = (dwAttr != 0xffffffff && (dwAttr & FILE_ATTRIBUTE_DIRECTORY));
			if (!bDirExists)
			{
				BOOL bRet = CreateDirectory(sPath.c_str(), NULL);
				if (bRet == FALSE) {
					MessageBox(0, "Error creating Domoticz directory in program data folder (%ProgramData%)!!", "Error:", MB_OK);
				}
			}
			sPath += "\\domoticz.db";
			dbasefile = sPath;
		}
	}
#endif
#endif

	if (cmdLine.HasSwitch("-dbase"))
	{
		if (cmdLine.GetArgumentCount("-dbase") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify a Database Name");
			return 1;
		}
		dbasefile = cmdLine.GetSafeArgument("-dbase", 0, "domoticz.db");
	}
	m_sql.SetDatabaseName(dbasefile);

	if (cmdLine.HasSwitch("-webroot"))
	{
		if (cmdLine.GetArgumentCount("-webroot") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify a web root path");
			return 1;
		}
		std::string szroot = cmdLine.GetSafeArgument("-webroot", 0, "");
		if (szroot.size() != 0)
			szWebRoot = szroot;
	}

	if (cmdLine.HasSwitch("-verbose"))
	{
		if (cmdLine.GetArgumentCount("-verbose") != 1)
		{
			_log.Log(LOG_ERROR, "Please specify a verbose level");
			return 1;
		}
		int Level = atoi(cmdLine.GetSafeArgument("-verbose", 0, "").c_str());
		m_mainworker.SetVerboseLevel((eVerboseLevel)Level);
	}
#if defined WIN32
	if (cmdLine.HasSwitch("-nobrowser"))
	{
		bStartWebBrowser = false;
	}
	//Init WinSock
	WSADATA data;
	WORD version;

	version = (MAKEWORD(2, 2));
	int ret = WSAStartup(version, &data);
	if (ret != 0)
	{
		ret = WSAGetLastError();

		if (ret == WSANOTINITIALISED)
		{
			_log.Log(LOG_ERROR, "Error: Winsock could not be initialized!");
		}
	}
	CoInitializeEx(0, COINIT_MULTITHREADED);
	CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
#endif
#ifndef WIN32
	if (cmdLine.HasSwitch("-daemon"))
	{
		g_bRunAsDaemon = true;
	}

	std::string daemonname = DAEMON_NAME;
	if (cmdLine.HasSwitch("-daemonname"))
	{
		daemonname = cmdLine.GetSafeArgument("-daemonname", 0, DAEMON_NAME);
	}

	std::string pidfile = PID_FILE;
	if (cmdLine.HasSwitch("-pidfile"))
	{
		pidfile = cmdLine.GetSafeArgument("-pidfile", 0, PID_FILE);
	}

	if (cmdLine.HasSwitch("-syslog"))
	{
		g_bUseSyslog = true;
		logfacname = cmdLine.GetSafeArgument("-syslog", 0, "");
		if ( logfacname.length() == 0 ) 
		{
			logfacname = "user";
		}
	}

	if ((g_bRunAsDaemon)||(g_bUseSyslog))
	{
		int idx, logfacility = 0;

		for ( idx = 0; idx < sizeof(facilities)/sizeof(facilities[0]); idx++ ) 
		{
			if (strcmp(facilities[idx].facname, logfacname.c_str()) == 0) 
			{
				logfacility = facilities[idx].facvalue;
				break;
			}
		} 
		if ( logfacility == 0 ) 
		{
			_log.Log(LOG_ERROR, "%s is an unknown syslog facility", logfacname.c_str());
			return 1;
		}

		// _log.Log(LOG_STATUS, "syslog to %s (%x)", logfacname.c_str(), logfacility);
		setlogmask(LOG_UPTO(LOG_INFO));
		openlog(daemonname.c_str(), LOG_CONS | LOG_PERROR, logfacility);

		syslog(LOG_INFO, "Domoticz is starting up....");
	}

	if (g_bRunAsDaemon)
	{
		/* Deamonize */
		daemonize(szStartupFolder.c_str(), pidfile.c_str());
	}
	if ((g_bRunAsDaemon) || (g_bUseSyslog))
	{
		syslog(LOG_INFO, "Domoticz running...");
	}
#endif

	if (!g_bRunAsDaemon)
	{
		signal(SIGINT, signal_handler);
		signal(SIGTERM, signal_handler);
	}

	if (!m_mainworker.Start())
	{
		return 1;
	}
	m_StartTime = time(NULL);

	/* now, lets get into an infinite loop of doing nothing. */
#if defined WIN32
#ifndef _DEBUG
	RedirectIOToConsole();	//hide console
#endif
	InitWindowsHelper(hInstance, hPrevInstance, nShowCmd, m_mainworker.GetWebserverAddress(), atoi(m_mainworker.GetWebserverPort().c_str()), bStartWebBrowser);
	MSG Msg;
	while (!g_bStopApplication)
	{
		if (PeekMessage(&Msg, NULL, 0, 0, PM_NOREMOVE))
		{
			if (GetMessage(&Msg, NULL, 0, 0) > 0)
			{
				TranslateMessage(&Msg);
				DispatchMessage(&Msg);
			}
		}
		else
			sleep_milliseconds(100);
	}
	TrayMessage(NIM_DELETE, NULL);
#else
	while ( !g_bStopApplication )
	{
		sleep_seconds(1);
	}
#endif
	_log.Log(LOG_STATUS, "Closing application!...");
	fflush(stdout);
	_log.Log(LOG_STATUS, "Stopping worker...");
	try
	{
		m_mainworker.Stop();
	}
	catch (...)
	{

	}
#ifndef WIN32
	if (g_bRunAsDaemon)
	{
		syslog(LOG_INFO, "Domoticz stopped...");
		daemonShutdown();

		// Delete PID file
		remove(pidfile.c_str());
	}
#else
	// Release WinSock
	WSACleanup();
	CoUninitialize();
#endif
	return 0;
}

