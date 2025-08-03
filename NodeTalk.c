// _______            __          _______         __  __
//|    |  |.-----..--|  |.-----. |_     _|.---.-.|  ||  |--.
//|       ||  _  ||  _  ||  -__|   |   |  |  _  ||  ||    <
//|__|____||_____||_____||_____|   |___|  |___._||__||__|__|
//                                              Author: ymkz

// File: node_talk.c
// ======================================================================
// Node Talk
// A lightweight CUI chat application for local networks on Windows.
// This single-file program can run as a server or a client.
//
// Features:
// - CUI-based, simple and lightweight.
// - Supports server/client modes.
// - Multi-threaded for asynchronous send/receive.
// - TCP communication (IPv4).
// - Supports user nicknames.
// - Notifies connection/disconnection.
// - Handles graceful shutdown with Ctrl+C and /quit command.
// - Added feature: Server discovery via UDP broadcast (`/list` command).
// - Added feature: Private messaging (`/w <nickname> <message>`).
// - Added feature: Command-line arguments for direct startup.
// - Added feature: Server can also send messages and participate in the chat.
//
// Technical Specifications:
// - Language: C (C99)
// - Communication: Winsock2 (Windows API)
// - Threads: Windows Threads
// - UI: Windows Console (Standard I/O)
//
// Cross-compilation from macOS using MinGW-w64 is possible.
// Build command example:
// x86_64-w64-mingw32-gcc node_talk.c -o NodeTalk.exe -lws2_32 -static
//
// ======================================================================
// Author: ymkz
// License: Creative Commons Attribution-NonCommercial 4.0 International (CC BY-NC 4.0)
//
// You are free to:
// - Share: copy and redistribute the material in any medium or format
// - Adapt: remix, transform, and build upon the material
//
// Under the following terms:
// - Attribution: You must give appropriate credit, provide a link to the license, 
//   and indicate if changes were made.
// - NonCommercial: You may not use the material for commercial purposes.
//
// Full license text: https://creativecommons.org/licenses/by-nc/4.0/
// ======================================================================



#include <stdio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tchar.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")

#define VERSION "2.00"
#define MAX_BUFFER_SIZE 1024
#define MAX_NAME_SIZE 32
#define MAX_CLIENTS 10
#define UDP_PORT 5555 // Port for server discovery
#define DISCOVERY_MESSAGE "NODETALK_DISCOVERY"

// Forward declarations for functions to resolve compilation errors
int start_server(const char* port_str_arg);
int start_client(const char* ip_address_arg, const char* port_str_arg);
DWORD WINAPI receive_thread_func(LPVOID lpParam);
DWORD WINAPI accept_clients_thread_func(LPVOID lpParam);
DWORD WINAPI discover_servers_thread_func(LPVOID lpParam);
DWORD WINAPI discovery_reply_thread_func(LPVOID lpParam);

// Global variables for shared data and handles
static SOCKET g_main_socket = INVALID_SOCKET;
static char g_nickname[MAX_NAME_SIZE];
static volatile BOOL g_is_running = TRUE;

// A structure to hold client-specific data
typedef struct {
    SOCKET socket;
    char name[MAX_NAME_SIZE];
} ClientInfo;

// A simple dynamic array to manage client connections
static ClientInfo g_clients[MAX_CLIENTS];
static int g_client_count = 0;
static CRITICAL_SECTION g_cs_client_list;

// Function to safely clean up Winsock and threads
void cleanup() {
    // Set running flag to false to terminate all loops
    g_is_running = FALSE;

    // Shutdown and close all client sockets if in server mode
    EnterCriticalSection(&g_cs_client_list);
    for (int i = 0; i < g_client_count; i++) {
        if (g_clients[i].socket != INVALID_SOCKET) {
            shutdown(g_clients[i].socket, SD_BOTH);
            closesocket(g_clients[i].socket);
        }
    }
    LeaveCriticalSection(&g_cs_client_list);

    // Clean up main socket
    if (g_main_socket != INVALID_SOCKET) {
        shutdown(g_main_socket, SD_BOTH);
        closesocket(g_main_socket);
        g_main_socket = INVALID_SOCKET;
    }

    DeleteCriticalSection(&g_cs_client_list);
    WSACleanup();
}

// Ctrl+C signal handler for graceful shutdown
BOOL WINAPI ctrl_handler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT) {
        printf("\n[INFO] Ctrl+C detected. Exiting gracefully...\n");
        cleanup();
        return TRUE;
    }
    return FALSE;
}

// Broadcasts a message to all connected clients except the sender
// sender_socket = INVALID_SOCKET will broadcast to all clients
void broadcast_message(SOCKET sender_socket, const char* message) {
    EnterCriticalSection(&g_cs_client_list);
    for (int i = 0; i < g_client_count; i++) {
        if (g_clients[i].socket != sender_socket) {
            send(g_clients[i].socket, message, (int)strlen(message), 0);
        }
    }
    LeaveCriticalSection(&g_cs_client_list);
}

// Sends a private message to a specific user.
// This function is now used by both server and client side.
void private_message(SOCKET source_socket, const char* sender_name, const char* recipient_name, const char* message) {
    EnterCriticalSection(&g_cs_client_list);
    for (int i = 0; i < g_client_count; i++) {
        if (strcmp(g_clients[i].name, recipient_name) == 0) {
            char private_msg[MAX_BUFFER_SIZE + MAX_NAME_SIZE + 20];
            sprintf(private_msg, "[PRIVATE from %s] %s", sender_name, message);
            send(g_clients[i].socket, private_msg, (int)strlen(private_msg), 0);
            printf("[INFO] Sent private message to '%s'.\n", recipient_name);
            LeaveCriticalSection(&g_cs_client_list);
            return;
        }
    }
    LeaveCriticalSection(&g_cs_client_list);
    printf("[ERROR] User '%s' not found or not connected.\n", recipient_name);
}

// Thread function for receiving messages from a specific client or the server
DWORD WINAPI receive_thread_func(LPVOID lpParam) {
    SOCKET target_socket = (SOCKET)lpParam;
    int i_result;
    char recv_buf[MAX_BUFFER_SIZE];
    char sender_name[MAX_NAME_SIZE] = "unknown";
    BOOL is_server_side = FALSE;

    // If this is a server-side thread, find the client's name.
    // Otherwise, it's a client thread.
    EnterCriticalSection(&g_cs_client_list);
    if (g_client_count > 0) {
        is_server_side = TRUE;
        for (int i = 0; i < g_client_count; i++) {
            if (g_clients[i].socket == target_socket) {
                strcpy(sender_name, g_clients[i].name);
                break;
            }
        }
    }
    LeaveCriticalSection(&g_cs_client_list);

    // Main receive loop for this socket
    while (g_is_running) {
        memset(recv_buf, 0, sizeof(recv_buf));
        i_result = recv(target_socket, recv_buf, MAX_BUFFER_SIZE, 0);

        if (i_result > 0) {
            recv_buf[i_result] = '\0';
            printf("\r<%s> %s\n", sender_name, recv_buf);
            printf("<%s> ", g_nickname);
            fflush(stdout);

            if (is_server_side) {
                // If it's a server thread, check for private message
                if (strncmp(recv_buf, "/w ", 3) == 0) {
                    char* token = strtok(recv_buf + 3, " ");
                    if (token != NULL) {
                        char recipient_name[MAX_NAME_SIZE];
                        strcpy(recipient_name, token);
                        char* message = strtok(NULL, "");
                        if (message != NULL) {
                            private_message(target_socket, sender_name, recipient_name, message);
                        }
                    }
                } else {
                    // Otherwise, broadcast the message
                    char broadcast_message_buf[MAX_BUFFER_SIZE + MAX_NAME_SIZE + 4];
                    sprintf(broadcast_message_buf, "<%s> %s", sender_name, recv_buf);
                    broadcast_message(target_socket, broadcast_message_buf);
                }
            }
        } else if (i_result == 0) {
            printf("\r[INFO] Connection closed by peer.\n");
            fflush(stdout);
            
            if (is_server_side) {
                EnterCriticalSection(&g_cs_client_list);
                for (int i = 0; i < g_client_count; i++) {
                    if (g_clients[i].socket == target_socket) {
                        printf("[INFO] Client '%s' disconnected.\n", g_clients[i].name);
                        closesocket(g_clients[i].socket);
                        char notif_msg[MAX_BUFFER_SIZE];
                        sprintf(notif_msg, "[INFO] '%s' has left the chat.", g_clients[i].name);
                        broadcast_message(INVALID_SOCKET, notif_msg);
                        for (int j = i; j < g_client_count - 1; j++) {
                            g_clients[j] = g_clients[j+1];
                        }
                        g_client_count--;
                        break;
                    }
                }
                LeaveCriticalSection(&g_cs_client_list);
            } else {
                 g_is_running = FALSE;
            }
            break;
        } else {
            if (g_is_running) {
                fprintf(stderr, "\r[ERROR] recv failed with error: %d\n", WSAGetLastError());
                g_is_running = FALSE;
                break;
            }
        }
    }
    return 0;
}

// Server mode implementation
int start_server(const char* port_str_arg) {
    printf("=== Node Talk ===\n");
    printf("Server mode\n");
    
    char host_name[256];
    if (gethostname(host_name, sizeof(host_name)) == 0) {
        struct hostent* host_info = gethostbyname(host_name);
        if (host_info != NULL) {
            struct in_addr addr;
            memcpy(&addr, host_info->h_addr_list[0], sizeof(struct in_addr));
            printf("IP Address: %s\n", inet_ntoa(addr));
        }
    }
    
    char port_str[8];
    if (port_str_arg) {
        strcpy(port_str, port_str_arg);
    }

    SOCKET listen_socket = INVALID_SOCKET;
    int i_result;
    
    do {
        if (!port_str_arg) {
            printf("Enter listening port number: ");
            if (fgets(port_str, sizeof(port_str), stdin) == NULL) {
                return 1;
            }
            port_str[strcspn(port_str, "\n")] = 0;
        }

        struct addrinfo hints, *result = NULL;
        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_PASSIVE;

        i_result = getaddrinfo(NULL, port_str, &hints, &result);
        if (i_result != 0) {
            fprintf(stderr, "[ERROR] getaddrinfo failed with error: %d\n", i_result);
            return 1;
        }

        listen_socket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (listen_socket == INVALID_SOCKET) {
            fprintf(stderr, "[ERROR] socket failed with error: %d\n", WSAGetLastError());
            freeaddrinfo(result);
            return 1;
        }

        i_result = bind(listen_socket, result->ai_addr, (int)result->ai_addrlen);
        if (i_result == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEADDRINUSE) {
                printf("That port is already in use by another application.\n");
                closesocket(listen_socket);
                listen_socket = INVALID_SOCKET;
                if (port_str_arg) {
                    return 1;
                }
            } else {
                fprintf(stderr, "[ERROR] bind failed with error: %d\n", WSAGetLastError());
                freeaddrinfo(result);
                closesocket(listen_socket);
                return 1;
            }
        }
        freeaddrinfo(result);
    } while (listen_socket == INVALID_SOCKET);

    i_result = listen(listen_socket, SOMAXCONN);
    if (i_result == SOCKET_ERROR) {
        fprintf(stderr, "[ERROR] listen failed with error: %d\n", WSAGetLastError());
        closesocket(listen_socket);
        return 1;
    }

    printf("[INFO] Waiting for client connections...\n");
    InitializeCriticalSection(&g_cs_client_list);

    g_main_socket = listen_socket;

    // Start a thread to accept new client connections
    _beginthreadex(NULL, 0, (_beginthreadex_proc_type)accept_clients_thread_func, (void*)listen_socket, 0, NULL);
    
    // Start a thread to reply to server discovery messages via UDP
    _beginthreadex(NULL, 0, (_beginthreadex_proc_type)discovery_reply_thread_func, NULL, 0, NULL);

    // Server's main input loop
    char send_buf[MAX_BUFFER_SIZE];
    while (g_is_running) {
        printf("<%s> ", g_nickname);
        if (fgets(send_buf, MAX_BUFFER_SIZE, stdin) != NULL) {
            send_buf[strcspn(send_buf, "\n")] = 0;
            
            if (strcmp(send_buf, "/quit") == 0) {
                printf("[INFO] Quitting...\n");
                g_is_running = FALSE;
                break;
            }
            
            if (strlen(send_buf) > 0) {
                if (strncmp(send_buf, "/w ", 3) == 0) {
                    char* token = strtok(send_buf + 3, " ");
                    if (token != NULL) {
                        char recipient_name[MAX_NAME_SIZE];
                        strcpy(recipient_name, token);
                        char* message = strtok(NULL, "");
                        if (message != NULL) {
                            private_message(INVALID_SOCKET, g_nickname, recipient_name, message);
                        } else {
                            printf("[ERROR] Usage: /w <nickname> <message>\n");
                        }
                    } else {
                        printf("[ERROR] Usage: /w <nickname> <message>\n");
                    }
                } else {
                    char broadcast_message_buf[MAX_BUFFER_SIZE + MAX_NAME_SIZE + 4];
                    sprintf(broadcast_message_buf, "<%s> %s", g_nickname, send_buf);
                    broadcast_message(INVALID_SOCKET, broadcast_message_buf);
                }
            }
        } else {
            g_is_running = FALSE;
        }
    }
    closesocket(listen_socket);
    return 0;
}

// Thread to accept incoming client connections
DWORD WINAPI accept_clients_thread_func(LPVOID lpParam) {
    SOCKET listen_socket = (SOCKET)lpParam;
    int i_result;

    while (g_is_running) {
        SOCKET client_socket = INVALID_SOCKET;
        client_socket = accept(listen_socket, NULL, NULL);
        if (client_socket == INVALID_SOCKET) {
            if (g_is_running) {
                 fprintf(stderr, "[ERROR] accept failed with error: %d\n", WSAGetLastError());
            }
            break;
        }

        EnterCriticalSection(&g_cs_client_list);
        if (g_client_count < MAX_CLIENTS) {
            char client_name[MAX_NAME_SIZE];
            i_result = recv(client_socket, client_name, MAX_NAME_SIZE - 1, 0);
            if (i_result > 0) {
                client_name[i_result] = '\0';
                g_clients[g_client_count].socket = client_socket;
                strcpy(g_clients[g_client_count].name, client_name);
                g_client_count++;
                
                printf("[INFO] New client '%s' connected. Total clients: %d\n", client_name, g_client_count);

                char notif_msg[MAX_BUFFER_SIZE];
                sprintf(notif_msg, "[INFO] '%s' has joined the chat.", client_name);
                broadcast_message(INVALID_SOCKET, notif_msg);

                _beginthreadex(NULL, 0, (_beginthreadex_proc_type)receive_thread_func, (void*)client_socket, 0, NULL);
            } else {
                fprintf(stderr, "[ERROR] Failed to receive client name. Disconnecting new client.\n");
                closesocket(client_socket);
            }
        } else {
            printf("[WARNING] Maximum clients reached. Rejecting new connection.\n");
            closesocket(client_socket);
        }
        LeaveCriticalSection(&g_cs_client_list);
    }
    return 0;
}

// Thread function to discover servers on the local network
DWORD WINAPI discover_servers_thread_func(LPVOID lpParam) {
    SOCKET discovery_socket = INVALID_SOCKET;
    struct sockaddr_in server_addr;
    char recv_buf[MAX_BUFFER_SIZE];
    int i_result;

    discovery_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (discovery_socket == INVALID_SOCKET) {
        fprintf(stderr, "[ERROR] UDP socket creation failed: %d\n", WSAGetLastError());
        return 1;
    }

    BOOL bOptVal = TRUE;
    if (setsockopt(discovery_socket, SOL_SOCKET, SO_BROADCAST, (char*)&bOptVal, sizeof(bOptVal)) == SOCKET_ERROR) {
        fprintf(stderr, "[ERROR] setsockopt failed: %d\n", WSAGetLastError());
        closesocket(discovery_socket);
        return 1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(UDP_PORT);
    server_addr.sin_addr.s_addr = INADDR_BROADCAST;

    i_result = sendto(discovery_socket, DISCOVERY_MESSAGE, (int)strlen(DISCOVERY_MESSAGE), 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (i_result == SOCKET_ERROR) {
        fprintf(stderr, "[ERROR] sendto failed: %d\n", WSAGetLastError());
        closesocket(discovery_socket);
        return 1;
    }

    printf("[INFO] Searching for servers...\n");

    DWORD timeout = 2000;
    setsockopt(discovery_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    struct sockaddr_in from_addr;
    int from_len = sizeof(from_addr);
    
    while (g_is_running) {
        i_result = recvfrom(discovery_socket, recv_buf, MAX_BUFFER_SIZE, 0, (struct sockaddr*)&from_addr, &from_len);
        if (i_result > 0) {
            recv_buf[i_result] = '\0';
            printf("Found server: %s:%s\n", inet_ntoa(from_addr.sin_addr), recv_buf);
        } else if (i_result == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT) {
            printf("[INFO] Server search complete.\n");
            break;
        }
    }

    closesocket(discovery_socket);
    return 0;
}

// Client mode implementation
int start_client(const char* ip_address_arg, const char* port_str_arg) {
    printf("=== Node Talk ===\n");
    printf("Client mode\n");
    
    char ip_address[16];
    char port_str[8];

    if (ip_address_arg && port_str_arg) {
        strcpy(ip_address, ip_address_arg);
        strcpy(port_str, port_str_arg);
    } else {
        printf("Enter server IP address: ");
        if (fgets(ip_address, sizeof(ip_address), stdin) == NULL) {
            return 1;
        }
        ip_address[strcspn(ip_address, "\n")] = 0;

        printf("Enter port number: ");
        if (fgets(port_str, sizeof(port_str), stdin) == NULL) {
            return 1;
        }
        port_str[strcspn(port_str, "\n")] = 0;
    }

    struct addrinfo hints, *result = NULL, *ptr = NULL;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    int i_result = getaddrinfo(ip_address, port_str, &hints, &result);
    if (i_result != 0) {
        fprintf(stderr, "[ERROR] getaddrinfo failed with error: %d\n", i_result);
        return 1;
    }

    g_main_socket = INVALID_SOCKET;
    for (ptr = result; ptr != NULL; ptr = ptr->ai_next) {
        g_main_socket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (g_main_socket == INVALID_SOCKET) {
            fprintf(stderr, "[ERROR] socket failed with error: %d\n", WSAGetLastError());
            continue;
        }

        i_result = connect(g_main_socket, ptr->ai_addr, (int)ptr->ai_addrlen);
        if (i_result == SOCKET_ERROR) {
            closesocket(g_main_socket);
            g_main_socket = INVALID_SOCKET;
            continue;
        }
        break;
    }

    freeaddrinfo(result);

    if (g_main_socket == INVALID_SOCKET) {
        fprintf(stderr, "[ERROR] Unable to connect to server!\n");
        return 1;
    }

    printf("[INFO] Connected to server.\n");

    i_result = send(g_main_socket, g_nickname, (int)strlen(g_nickname), 0);
    if (i_result == SOCKET_ERROR) {
        fprintf(stderr, "[ERROR] Failed to send nickname to server: %d\n", WSAGetLastError());
        return 1;
    }

    _beginthreadex(NULL, 0, (_beginthreadex_proc_type)receive_thread_func, (void*)g_main_socket, 0, NULL);
    
    char send_buf[MAX_BUFFER_SIZE];
    while (g_is_running) {
        printf("<%s> ", g_nickname);
        if (fgets(send_buf, MAX_BUFFER_SIZE, stdin) != NULL) {
            send_buf[strcspn(send_buf, "\n")] = 0;
            
            if (strcmp(send_buf, "/quit") == 0) {
                printf("[INFO] Quitting...\n");
                g_is_running = FALSE;
                break;
            }
            
            if (strlen(send_buf) > 0) {
                if (strncmp(send_buf, "/w ", 3) == 0) {
                    // Private message logic
                    char* token = strtok(send_buf + 3, " ");
                    if (token != NULL) {
                        char recipient_name[MAX_NAME_SIZE];
                        strcpy(recipient_name, token);
                        char* message = strtok(NULL, "");
                        if (message != NULL) {
                            char whisper_msg[MAX_BUFFER_SIZE];
                            sprintf(whisper_msg, "/w %s %s", recipient_name, message);
                            i_result = send(g_main_socket, whisper_msg, (int)strlen(whisper_msg), 0);
                            if (i_result == SOCKET_ERROR) {
                                fprintf(stderr, "[ERROR] send failed with error: %d\n", WSAGetLastError());
                                g_is_running = FALSE;
                                break;
                            }
                        } else {
                            printf("[ERROR] Usage: /w <nickname> <message>\n");
                        }
                    } else {
                        printf("[ERROR] Usage: /w <nickname> <message>\n");
                    }
                } else if (strcmp(send_buf, "/list") == 0) {
                    _beginthreadex(NULL, 0, (_beginthreadex_proc_type)discover_servers_thread_func, NULL, 0, NULL);
                } else {
                    i_result = send(g_main_socket, send_buf, (int)strlen(send_buf), 0);
                    if (i_result == SOCKET_ERROR) {
                        fprintf(stderr, "[ERROR] send failed with error: %d\n", WSAGetLastError());
                        g_is_running = FALSE;
                        break;
                    }
                }
            }
        } else {
            g_is_running = FALSE;
        }
    }

    return 0;
}

// Main function
int main(int argc, char* argv[]) {
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    printf("===NodeTalk===\n");
    printf("Local Network Chat\n");
    printf("v=%s\n\n", VERSION);

    WSADATA wsa_data;
    int i_result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (i_result != 0) {
        fprintf(stderr, "[ERROR] WSAStartup failed with error: %d\n", i_result);
        return 1;
    }

    printf("Enter your name: ");
    if (fgets(g_nickname, MAX_NAME_SIZE, stdin) == NULL) {
        cleanup();
        return 1;
    }
    g_nickname[strcspn(g_nickname, "\n")] = 0;

    if (argc >= 3) {
        if (strcmp(argv[1], "-s") == 0) {
            start_server(argv[2]);
        } else if (strcmp(argv[1], "-c") == 0) {
            char* colon = strchr(argv[2], ':');
            if (colon) {
                *colon = '\0';
                start_client(argv[2], colon + 1);
            } else {
                fprintf(stderr, "[ERROR] Invalid client connection string. Usage: NodeTalk.exe -c <ip>:<port>\n");
            }
        }
    } else {
        int choice;
        while (g_is_running) {
            printf("\n=== Node Talk ===\n");
            printf("1. Start as Server\n");
            printf("2. Connect as Client\n");
            printf("3. Exit\n");
            printf("Enter your choice: ");

            if (scanf("%d", &choice) != 1) {
                int c;
                while ((c = getchar()) != '\n' && c != EOF);
                printf("Invalid input. Please enter a number.\n");
                continue;
            }
            
            int c;
            while ((c = getchar()) != '\n' && c != EOF);

            switch (choice) {
                case 1:
                    start_server(NULL);
                    break;
                case 2:
                    start_client(NULL, NULL);
                    break;
                case 3:
                    printf("[INFO] Exiting...\n");
                    g_is_running = FALSE;
                    break;
                default:
                    printf("Invalid choice. Please enter 1, 2, or 3.\n");
                    continue;
            }
            if (!g_is_running) {
                break;
            }
        }
    }

    cleanup();
    return 0;
}

// UDP thread to reply to discovery messages
DWORD WINAPI discovery_reply_thread_func(LPVOID lpParam) {
    SOCKET discovery_socket = INVALID_SOCKET;
    struct sockaddr_in server_addr;
    char recv_buf[MAX_BUFFER_SIZE];
    int i_result;

    discovery_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (discovery_socket == INVALID_SOCKET) {
        fprintf(stderr, "[ERROR] UDP socket creation failed: %d\n", WSAGetLastError());
        return 1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(UDP_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    i_result = bind(discovery_socket, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (i_result == SOCKET_ERROR) {
        fprintf(stderr, "[ERROR] UDP bind failed: %d\n", WSAGetLastError());
        closesocket(discovery_socket);
        return 1;
    }

    struct sockaddr_in from_addr;
    int from_len = sizeof(from_addr);

    while (g_is_running) {
        i_result = recvfrom(discovery_socket, recv_buf, MAX_BUFFER_SIZE, 0, (struct sockaddr*)&from_addr, &from_len);
        if (i_result > 0) {
            recv_buf[i_result] = '\0';
            if (strcmp(recv_buf, DISCOVERY_MESSAGE) == 0) {
                char port_str[8];
                struct sockaddr_in tcp_addr;
                int tcp_addr_len = sizeof(tcp_addr);
                getsockname(g_main_socket, (struct sockaddr*)&tcp_addr, &tcp_addr_len);
                sprintf(port_str, "%d", ntohs(tcp_addr.sin_port));
                sendto(discovery_socket, port_str, (int)strlen(port_str), 0, (struct sockaddr*)&from_addr, from_len);
            }
        }
    }
    closesocket(discovery_socket);
    return 0;
}
