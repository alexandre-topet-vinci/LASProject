#include "utils_v3.h"
#include "game.h"
#include <stdlib.h>
#include <fcntl.h>

#define KEY 84937
#define PERM 0666

#define SERVER_IP "127.0.0.1"
#define LOCAL_HOST "127.0.0.1"
#define SERVER_PORT 74912
#define BACKLOG 5

int initSocketClient(char * serverIP, int serverPort)
{
    int sockfd = ssocket();
    sconnect(serverIP, serverPort, sockfd);
    return sockfd;
}

int initSocketServer(int serverPort){
    int sockfd = ssocket();
    sbind(serverPort, sockfd);
    slisten(sockfd, BACKLOG);
    return sockfd;
}

void client_handler(int sig) {
    // Handler pour le signal SIGUSR1  
    int sockfd = initSocketClient(SERVER_IP, SERVER_PORT);

    int shm_id = sshmget(KEY, sizeof(struct GameState), IPC_CREAT | PERM);
    struct GameState *state = sshmat(shm_id);

    // Vous pouvez ajouter du code ici si nécessaire    

    sshmdt(state); 
}

int main(int argc, char *argv[]) {
    int shm_id = sshmget(KEY, sizeof(struct GameState), IPC_CREAT | PERM);
    struct GameState *state = sshmat(shm_id);

    int sockfd = initSocketServer(SERVER_PORT);    

    int pipefdServerToBroadcast[2];
    spipe(pipefdServerToBroadcast);
    
    FileDescriptor sout = 1;
    FileDescriptor map  = sopen("./resources/map.txt", O_RDONLY, 0);
    load_map(map, sout, state);
    sclose(map);

    // implémentation de la logique du jeu ici

    pid_t client1 = sfork();
    if (client1 == 0) {
        sclose(pipefdServerToBroadcast[0]);
        
        client_handler(0); // Passing 0 as a dummy signal value
        exit(EXIT_SUCCESS);
    }
    
    pid_t client2 = sfork();
    if (client2 == 0) {
        sclose(pipefdServerToBroadcast[0]);
        
        client_handler(0); // Passing 0 as a dummy signal value
        exit(EXIT_SUCCESS);
    }
    pid_t broadcast = sfork();
    if (broadcast == 0) {   
        sclose(pipefdServerToBroadcast[1]);
        // Code pour le processus de diffusion
        int newsockfd = saccept(sockfd); 
        
        exit(EXIT_SUCCESS);
    }

    swaitpid(client1, NULL, 0);
    swaitpid(client2, NULL, 0);

    sshmdt(state);
    sshmdelete(shm_id);
    return 0;
}