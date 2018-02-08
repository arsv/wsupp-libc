#include <string.h>
#include <unistd.h>
#include <sys/file.h>

#include "common.h"
#include "control.h"
#include "nlusctl.h"
#include "crypto/pbkdf2.h"
#include "wifi.h"

static void put_psk(CTX, uint8_t* ssid, int slen, char* pass, int plen)
{
	uint8_t psk[32];

	memzero(psk, sizeof(psk));

	pbkdf2_sha1(psk, sizeof(psk), pass, plen, ssid, slen, 4096);

	uc_put_bin(UC, ATTR_PSK, psk, sizeof(psk));
}

static int input_passphrase(char* buf, int len)
{
	int rd;
	char* prompt = "Passphrase: ";

	write(STDOUT, prompt, strlen(prompt));
	rd = read(STDIN, buf, len);

	if(rd >= len)
		fail("passphrase too long\n");

	if(rd > 0 && buf[rd-1] == '\n')
		rd--;
	if(!rd)
		fail("empty passphrase rejected\n");

	buf[rd] = '\0';

	return rd;
}

/* As written this only works with sane ssids and passphrases.
   Should be extended at some point to handle ssid escapes and
   multiline phrases. */

void put_psk_input(CTX, void* ssid, int slen)
{
	char buf[256];
	int len = input_passphrase(buf, sizeof(buf));

	put_psk(ctx, ssid, slen, buf, len);
}
