#include "args.h"

#include "nis.h"
#include "posix_wait.h"
#include "posix_ifos.h"
#include "posix_naos.h"
#include "logger.h"

#include <stdio.h>

#define ECHO(fmt, arg...) log__save("nshost.echo", kLogLevel_Info, kLogTarget_Stdout | kLogTarget_Filesystem, fmt, ##arg)

int display(HTCPLINK link, const unsigned char *data, int size)
{
	char output[1024];
	uint32_t ip;
	uint16_t port;
	char ipstr[INET_ADDRSTRLEN];
	int offset;

	tcp_getaddr(link, LINK_ADDR_REMOTE, &ip, &port);
	posix__ipv4tos(ip, ipstr, sizeof(ipstr));
	offset = sprintf(output, "[income %s:%u] ", ipstr, port);

	if (size < (sizeof(output) - offset) && size > 0) {
		memcpy(&output[offset], data, size);
		return posix__file_write(1, output, size + offset);
	}

	return -1;
}

void tcp_server_callback(const struct nis_event *event, const void *data)
{
	struct nis_tcp_data *tcpdata = (struct nis_tcp_data *)data;
	HTCPLINK link;

	link = event->Ln.Tcp.Link;
	switch(event->Event) {
		case EVT_RECEIVEDATA:
			if (display(link, tcpdata->e.Packet.Data, tcpdata->e.Packet.Size) > 0 ) {
				if (tcp_write(link, tcpdata->e.Packet.Data, tcpdata->e.Packet.Size, NULL) <= 0) {
					tcp_destroy(link);
				}
			} else {
				tcp_destroy(link);
			}
			break;
		case EVT_TCP_ACCEPTED:
			break;
		case EVT_CLOSED:
			break;
		default:
			break;
	}
}

void tcp_client_callback(const struct nis_event *event, const void *data)
{
	struct nis_tcp_data *tcpdata = (struct nis_tcp_data *)data;

	switch(event->Event) {
		case EVT_RECEIVEDATA:
			display(event->Ln.Tcp.Link, tcpdata->e.Packet.Data, tcpdata->e.Packet.Size);
			posix__file_write(1, "input:$ ", 8);
			break;
		case EVT_TCP_ACCEPTED:
			break;
		case EVT_CLOSED:
			break;
		case EVT_TCP_CONNECTED:
			posix__file_write(1, "input:$ ", 8);
			break;
		default:
			break;
	}
}

void nshost_ecr(const char *host_event, const char *reserved, int rescb)
{
	if (host_event) {
		ECHO("%s", host_event);
	}
}

int echo_server_startup(const char *host, uint16_t port)
{
	HTCPLINK server;

	server = tcp_create(&tcp_server_callback, host, port);
	if (INVALID_HTCPLINK == server) {
		return 1;
	}

	tcp_listen(server, 100);
	posix__hang();
	return 0;
}

int echo_client_startup(const char *host, uint16_t port)
{
	HTCPLINK client;
	char text[165536], *p;
	size_t n;

	do {
		client = tcp_create(&tcp_client_callback, NULL, 0);
		if (INVALID_HTCPLINK == client) {
			break;
		}

		if (tcp_connect2(client, host, port) < 0) {
			break;
		}

		n = 165536;
		while(1) {
			tcp_write(client, text, n, NULL);
		}
		// while ( NULL != (p = fgets(text, sizeof(text), stdin)) ) {
		// 	n = strlen(text);
		// 	if (tcp_write(client, text, n, NULL) < 0) {
		// 		break;
		// 	}
		// }
	} while( 0 );

	return 1;
}

int echo_client_startup2(const char *host, uint16_t port)
{
	HTCPLINK client[2];
	int i;

	for (i = 0; i < 2; i++) {
		client[i] = tcp_create(&tcp_client_callback, NULL, 0);
		if (INVALID_HTCPLINK == client[i]) {
			break;
		}

		if (tcp_connect2(client[i], host, port) < 0) {
			break;
		}
	}

	posix__hang();
	return 1;
}

#define _SET_ECR 1

int main(int argc, char **argv)
{
	int type;

	if (check_args(argc, argv) < 0) {
		return -1;
	}

	if ((type = gettype()) < 0 ){
		return 1;
	}

	log__init();
	tcp_init();

#if _SET_ECR
	nis_checr(&nshost_ecr);
#endif

	if (type == SESS_TYPE_SERVER) {
		return echo_server_startup(gethost(), getport());
	}

	if (type == SESS_TYPE_CLIENT) {
		return echo_client_startup(gethost(), getport());
	}

	return 0;
}