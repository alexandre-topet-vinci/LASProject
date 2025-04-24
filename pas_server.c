#include "utils_v3.h"

#define KEY 84937
#define PERM 0666

#define SERVER_PORT 74912

void client_handler(int sig) {
    // Handler pour le signal SIGUSR1  
    int shm_id = sshmget(KEY, sizeof(int), IPC_CREAT | PERM);
    int *distance = sshmat(shm_id);

    // Vous pouvez ajouter du code ici si nécessaire    

    sshmdt(distance); 
}

int initSocketServer(int serverPort){
            int sockfd = ssocket();
            sbind(serverPort, sockfd);
            slisten(sockfd, BACKLOG);
            return sockfd;
}

int main(int argc, char *argv[]) {
    int shm_id = sshmget(KEY, sizeof(int), IPC_CREAT | PERM);
    int *valeur = sshmat(shm_id);

    int pipefdServerToBroadcast[2];
    spipe(pipefdServerToBroadcast);
    struct GameState state;
    FileDescriptor sout = 1;
    FileDescriptor map  = sopen("./resources/map.txt", O_RDONLY, 0);
    load_map(map, sout, &state);
    sclose(map);

    // implémentation de la logique du jeu ici

    pid_t client1 = sfork();
    if (client1 == 0) {
        sclose(pipefdServerToBroadcast[0]);
        
        client_handler();
        exit(EXIT_SUCCESS);
    }
    
    pid_t client2 = sfork();
    if (client2 == 0) {
        sclose(pipefdServerToBroadcast[0]);
        
        client_handler();
        exit(EXIT_SUCCESS);
    }
    pid_t broadcast = sfork();
    if (broadcast == 0) {   
        sclose(pipefdServerToBroadcast[1]);
        // Code pour le processus de diffusion
        int sockfd = initSocketServer(SERVER_PORT);
        
        exit(EXIT_SUCCESS);
    }

    swaitpid(client1, NULL, 0);
    swaitpid(client2, NULL, 0);

    sshmdt(valeur);
    sshmdelete(shm_id);
    return 0;
}