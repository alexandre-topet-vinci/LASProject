#include "utils_v3.h"
#include "game.h"

#define KEY 84937
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 74912

int initSocketClient(char *serverIP, int serverPort) {
    int sockfd = ssocket();
    sconnect(serverIP, serverPort, sockfd);
    return sockfd;
}

void server_handler(int sig) {
    // Handler for signals from server
    // You can implement the necessary logic here
}

int main(int argc, char *argv[]) {
    // Connect to server
    int sockfd = initSocketClient(SERVER_IP, SERVER_PORT);
    
    // Set up shared memory
    int shm_id = sshmget(KEY, sizeof(int), IPC_CREAT | 0666);
    int *shared_data = sshmat(shm_id);
    
    // Set up signal handler
    struct sigaction sa;
    sa.sa_handler = server_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);
    
    // Game loop
    int running = 1;
    while (running) {
        // Handle user input
        // Send data to server
        // Receive and process game state
    }
    
    // Clean up
    sshmdt(shared_data);
    sclose(sockfd);
    
    return 0;
}