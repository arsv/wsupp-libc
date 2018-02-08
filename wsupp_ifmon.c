#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "common.h"
#include "wsupp.h"

static int dhcpid;

void kill_dhcp(void)
{
	if(dhcpid <= 0)
		return;

	kill(dhcpid, SIGTERM);
}

void reap_dhcp(void)
{
	int pid, status;

	if((pid = waitpid(-1, &status, WNOHANG)) <= 0)
		return;
	if(pid != dhcpid)
		return;

	dhcpid = 0;

	if(!status)
		return;
	if(WIFEXITED(status))
		warn("dhcp failed with code %i\n", WEXITSTATUS(status));
	if(WIFSIGNALED(status))
		warn("dhcp killed by signal %i\n", WTERMSIG(status));
}

void trigger_dhcp(void)
{
	int pid;

	kill_dhcp();

	if((pid = fork()) < 0)
		fail("fork: %m\n");

	if(pid == 0) {
		char* argv[] = { "dhcp", ifname, NULL };

		execv(*argv, argv);

		fail("exec %s: %m\n", *argv);
	}

	dhcpid = pid;
}
