#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#define PORT 2026

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Uso: %s <ip_servidor> [puerto]\n", argv[0]);
        return 1;
    }
    int port = (argc > 2) ? atoi(argv[2]) : PORT;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, argv[1], &serv_addr.sin_addr) <= 0) {
        perror("inet_pton"); return 1;
    }

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect"); return 1;
    }
    printf("Conectado al servidor. Escribe comandos:\n");

    fd_set readfds;
    char buf[1024];

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(sock, &readfds);

        if (select(sock + 1, &readfds, NULL, NULL, NULL) < 0) {
            perror("select"); break;
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (fgets(buf, sizeof(buf), stdin) == NULL) break;
            send(sock, buf, strlen(buf), 0);
            if (strncmp(buf, "SALIR", 5) == 0) break;
        }

        if (FD_ISSET(sock, &readfds)) {
            int n = recv(sock, buf, sizeof(buf) - 1, 0);
            if (n <= 0) {
                printf("Servidor desconectado.\n");
                break;
            }
            buf[n] = 0;
            printf("%s", buf);
            fflush(stdout);
        }
    }

    close(sock);
    return 0;
}
