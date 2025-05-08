#include "utils_v3.h"
#include "game.h"
#include "pascman.h"
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <time.h>
#include <string.h>
#include <stdbool.h>
#include <poll.h>

#define KEY 84937
#define PERM 0660
#define SERVER_PORT 74912
#define BACKLOG 5
#define REGISTRATION_TIMEOUT 30 // 30 seconds timeout for registration
#define MAX_CLIENTS 2
#define DEFAULT_MAP_FILE "./resources/map.txt"

// Semaphores
#define SEM_KEY 84938
#define SEM_MUTEX 0
#define SEM_SYNC 1

// Forward declaration for function not exposed in game.h
void send_game_over(enum Item winner, FileDescriptor fdbcast);

// Global variables for cleanup
int sockfd = -1;
int shm_id = -1;
int sem_id = -1;
int broadcast_pipe[2] = {-1, -1};
pid_t broadcaster_pid = -1;
pid_t client_handlers[MAX_CLIENTS] = {-1, -1};
bool running = true;
char *g_map_file = DEFAULT_MAP_FILE;

void cleanup() {
    // Kill client handlers
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_handlers[i] > 0) {
            skill(client_handlers[i], SIGTERM);
            waitpid(client_handlers[i], NULL, 0);
            client_handlers[i] = -1;
        }
    }
    
    // Kill broadcaster
    if (broadcaster_pid > 0) {
        skill(broadcaster_pid, SIGTERM);
        waitpid(broadcaster_pid, NULL, 0);
        broadcaster_pid = -1;
    }
    
    // Close pipes
    if (broadcast_pipe[0] != -1) {
        sclose(broadcast_pipe[0]);
        broadcast_pipe[0] = -1;
    }
    if (broadcast_pipe[1] != -1) {
        sclose(broadcast_pipe[1]);
        broadcast_pipe[1] = -1;
    }
    
    // Close server socket
    if (sockfd != -1) {
        sclose(sockfd);
        sockfd = -1;
    }
    
    // Clean up shared memory
    if (shm_id != -1) {
        sshmdelete(shm_id);
        shm_id = -1;
    }
    
    // Clean up semaphores
    if (sem_id != -1) {
        sem_delete(sem_id);
        sem_id = -1;
    }
    
    printf("Server cleaned up and exiting\n");
}

// Signal handler for SIGINT
void sigint_handler(int sig) {
    if (sig == SIGINT) {
        printf("SIGINT received, server will stop after current game\n");
        running = false;
    } else if (sig == SIGALRM) {
        printf("Connection timeout: No clients connected within 30 seconds\n");
        running = false;
    }
}

// Broadcaster process - reads messages from pipe and forwards to clients
void broadcaster_process(void* pipe_read_fd) {
    int read_fd = (int)(long)pipe_read_fd;
    
    // Set up polling for the broadcast pipe
    struct pollfd poll_fd;
    poll_fd.fd = read_fd;
    poll_fd.events = POLLIN;
    
    while (1) {
        // Poll for incoming messages
        int poll_result = spoll(&poll_fd, 1, -1);
        
        if (poll_result > 0 && (poll_fd.revents & POLLIN)) {
            // Nous allons lire le message complet
            union Message msg;
            memset(&msg, 0, sizeof(union Message));
            ssize_t bytes_read = sread(read_fd, &msg, sizeof(union Message));
            
            if (bytes_read <= 0) {
                break;  // Pipe closed
            }
            
            printf("Broadcaster: received message type %d\n", msg.msgt);
            
            // Dans le broadcaster, nous ne pouvons pas directement envoyer aux clients
            // car les sockets clients sont gérés par le processus principal
            // Nous allons donc juste traiter et afficher les messages
            
            switch (msg.msgt) {
                case SPAWN:
                    printf("Broadcaster: received SPAWN message for item %d at position (%u,%u)\n",
                          msg.spawn.item, msg.spawn.pos.x, msg.spawn.pos.y);
                    break;
                    
                case MOVEMENT:
                    printf("Broadcaster: received MOVEMENT message for item %u to position (%u,%u)\n",
                          msg.movement.id, msg.movement.pos.x, msg.movement.pos.y);
                    break;
                    
                case EAT_FOOD:
                    printf("Broadcaster: received EAT_FOOD message, eater %u ate food %u\n",
                          msg.eat_food.eater, msg.eat_food.food);
                    break;
                    
                case GAME_OVER:
                    printf("Broadcaster: received GAME_OVER message, winner is Player %d\n",
                          msg.game_over.winner);
                    break;
                    
                default:
                    printf("Broadcaster: received unhandled message type %d\n", msg.msgt);
                    break;
            }
        }
    }
    
    sclose(read_fd);
    exit(EXIT_SUCCESS);
}

// Structure pour stocker les messages à diffuser
struct BroadcastMessage {
    union Message msg;
    int client_count;
    int client_sockets[MAX_CLIENTS];
};

// Fonction pour diffuser un message à tous les clients connectés
void broadcast_to_clients(union Message *msg, int client_sockets[], int client_count) {
    for (int i = 0; i < client_count; i++) {
        if (client_sockets[i] != -1) {
            swrite(client_sockets[i], msg, sizeof(union Message));
        }
    }
    
    // Pour GAME_OVER, envoyer deux fois pour assurer la livraison
    if (msg->msgt == GAME_OVER) {
        usleep(100000); // Attendre 100ms
        for (int i = 0; i < client_count; i++) {
            if (client_sockets[i] != -1) {
                swrite(client_sockets[i], msg, sizeof(union Message));
            }
        }
    }
}

// Client handler process
void client_handler(int client_num, int client_socket) {
    // Register with the game interface
    send_registered(client_num, client_socket);
    
    // Attach to shared memory
    int shm_id = sshmget(KEY, sizeof(struct GameState), 0);
    struct GameState *state = sshmat(shm_id);
    
    // Set up semaphores
    int sem_id = sem_get(SEM_KEY, 2);
    
    enum Item player = (client_num == 1) ? PLAYER1 : PLAYER2;
    char direction_buffer[4];
    int game_running = 1;
    
    // Wait for both clients to be ready before Player 1 loads the map
    if (client_num == 1) {
        printf("Player 1 waiting for Player 2 to connect...\n");
        sem_down(sem_id, SEM_SYNC);  // Wait for Player 2 to signal
        
        // Add a small delay to ensure both clients are fully ready
        usleep(100000);  // 100ms
        
        // Load map and broadcast initial state
        printf("Loading map from %s\n", g_map_file);
        FileDescriptor map_fd = sopen(g_map_file, O_RDONLY, 0);
        if (map_fd >= 0) {
            load_map(map_fd, broadcast_pipe[1], state);
            sclose(map_fd);
            printf("Map loaded and sent to clients\n");
        } else {
            perror("Failed to open map file");
            exit(EXIT_FAILURE);
        }
    } else {
        // Player 2 signals readiness to player 1
        printf("Player 2 signaling readiness to Player 1\n");
        sem_up(sem_id, SEM_SYNC);  // Signal to Player 1
    }
    
    while (game_running && running) {
        // Wait for direction input from client
        ssize_t bytes_read = sread(client_socket, direction_buffer, sizeof(direction_buffer));
        if (bytes_read <= 0) {
            // Client disconnected or error
            printf("Client %d disconnected\n", client_num);
            break;
        }
        
        // Convert input to direction
        enum Direction dir;
        memcpy(&dir, direction_buffer, sizeof(dir));
        
        // Lock access to shared memory
        sem_down(sem_id, SEM_MUTEX);
        
        // Process the command and update game state
        game_running = !process_user_command(state, player, dir, broadcast_pipe[1]);
        
        // If game ended, send game over message
        if (!game_running || state->game_over) {
            // Determine the winner based on scores according to game rules
            enum Item winner_item = state->scores[0] > state->scores[1] ? PLAYER1 : PLAYER2;
            
            printf("Game over - Player %d wins with score %d vs %d\n", 
                winner_item == PLAYER1 ? 1 : 2,
                state->scores[winner_item == PLAYER1 ? 0 : 1],
                state->scores[winner_item == PLAYER1 ? 1 : 0]);
                
            // Envoyer le message GAME_OVER deux fois pour s'assurer qu'il est bien reçu
            send_game_over(winner_item, broadcast_pipe[1]);
            
            // Petit délai pour s'assurer que le premier message est traité
            usleep(100000);  // 100ms
            
            // Renvoyer le message pour s'assurer qu'il est bien reçu
            send_game_over(winner_item, broadcast_pipe[1]);
            
            game_running = 0;
        }
        
        // Unlock access to shared memory
        sem_up(sem_id, SEM_MUTEX);
    }
    
    sshmdt(state);
    sclose(client_socket);
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
    int port = SERVER_PORT;
    
    // Parse command line arguments if provided
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    if (argc > 2) {
        g_map_file = argv[2];
    }
    
    printf("Starting PAS-CMAN server on port %d using map %s...\n", port, g_map_file);
    
    // Set up signal handlers
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
    
    // Register atexit handler
    atexit(cleanup);
    
    // Initialize semaphore - two semaphores: mutex and sync
    sem_id = sem_create(SEM_KEY, 2, PERM, 1);
    sem_down(sem_id, SEM_SYNC);  // Initialize sync semaphore to 0
    
    // Set up shared memory
    shm_id = sshmget(KEY, sizeof(struct GameState), IPC_CREAT | PERM);
    struct GameState *state = sshmat(shm_id);
    reset_gamestate(state);
    
    // Create the broadcast pipe
    spipe(broadcast_pipe);
    
    // Create and set up server socket
    sockfd = ssocket();
    int option_value = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &option_value, sizeof(int));
    sbind(port, sockfd);
    slisten(sockfd, BACKLOG);
    
    printf("Server started on port %d, waiting for clients...\n", port);
    
    // Create a pipe for intercepting and forwarding broadcast messages
    int intercept_pipe[2];
    spipe(intercept_pipe);
    
    // Start broadcaster process - now it will write to intercept_pipe
    broadcaster_pid = sfork();
    if (broadcaster_pid == 0) {
        // Child process (broadcaster)
        sclose(broadcast_pipe[1]);  // Close write end
        sclose(intercept_pipe[0]);  // Close read end of intercept pipe
        
        // Set up polling for the broadcast pipe
        struct pollfd poll_fd;
        poll_fd.fd = broadcast_pipe[0];
        poll_fd.events = POLLIN;
        
        while (1) {
            // Poll for incoming messages
            int poll_result = spoll(&poll_fd, 1, -1);
            
            if (poll_result > 0 && (poll_fd.revents & POLLIN)) {
                // Nous allons lire le message complet
                union Message msg;
                memset(&msg, 0, sizeof(union Message));
                ssize_t bytes_read = sread(broadcast_pipe[0], &msg, sizeof(union Message));
                
                if (bytes_read <= 0) {
                    break;  // Pipe closed
                }
                
                // Log le message reçu
                printf("Broadcaster: received message type %d\n", msg.msgt);
                
                // Et le transférer au processus principal via le pipe d'interception
                swrite(intercept_pipe[1], &msg, sizeof(union Message));
            }
        }
        
        sclose(broadcast_pipe[0]);
        sclose(intercept_pipe[1]);
        exit(EXIT_SUCCESS);
    }
    
    // Parent process continues
    sclose(broadcast_pipe[0]);  // Close read end of broadcast_pipe
    sclose(intercept_pipe[1]);  // Close write end of intercept_pipe
    
    while (running) {
        printf("Waiting for players to connect...\n");
        
        // Game registration phase (30 seconds timeout)
        int client_sockets[MAX_CLIENTS] = {-1, -1};
        int client_count = 0;
        
        // Set timeout for client connections
        alarm(REGISTRATION_TIMEOUT);
        
        // Accept client connections
        while (client_count < MAX_CLIENTS && running) {
            int client_socket = saccept(sockfd);
            
            // Cancel the timeout since we got a connection
            alarm(0);
            
            client_sockets[client_count] = client_socket;
            client_count++;
            
            printf("Client %d connected\n", client_count);
            
            // Set new timeout if we need more clients
            if (client_count < MAX_CLIENTS) {
                alarm(REGISTRATION_TIMEOUT);
            }
        }
        
        // If we didn't get enough clients, disconnect and restart
        if (client_count < MAX_CLIENTS || !running) {
            printf("Not enough players connected or server stopping. Cleaning up.\n");
            for (int i = 0; i < client_count; i++) {
                sclose(client_sockets[i]);
            }
            
            if (!running) break;
            continue;  // Restart registration phase
        }
        
        // Reset game state for new game
        reset_gamestate(state);
        
        // Fork a process to handle the interception and forwarding of messages to clients
        pid_t forwarder_pid = sfork();
        
        if (forwarder_pid == 0) {
            // Child process (forwarder)
            printf("Message forwarder started\n");
            
            // Set up polling for interceptor pipe
            struct pollfd poll_fd;
            poll_fd.fd = intercept_pipe[0];
            poll_fd.events = POLLIN;
            
            while (1) {
                int poll_result = spoll(&poll_fd, 1, -1);
                
                if (poll_result > 0 && (poll_fd.revents & POLLIN)) {
                    union Message msg;
                    memset(&msg, 0, sizeof(union Message));
                    ssize_t bytes_read = sread(intercept_pipe[0], &msg, sizeof(union Message));
                    
                    if (bytes_read <= 0) {
                        break;  // Pipe closed
                    }
                    
                    // Envoyer le message à tous les clients
                    printf("Forwarder: transmitting message type %d to clients\n", msg.msgt);
                    
                    for (int i = 0; i < client_count; i++) {
                        if (client_sockets[i] != -1) {
                            if (swrite(client_sockets[i], &msg, sizeof(union Message)) < 0) {
                                perror("Failed to forward message to client");
                            }
                        }
                    }
                    
                    // Pour GAME_OVER, on envoie deux fois avec un délai
                    if (msg.msgt == GAME_OVER) {
                        usleep(100000); // 100ms
                        
                        for (int i = 0; i < client_count; i++) {
                            if (client_sockets[i] != -1) {
                                swrite(client_sockets[i], &msg, sizeof(union Message));
                            }
                        }
                        
                        printf("Forwarder: sent GAME_OVER message again\n");
                    }
                }
            }
            
            exit(EXIT_SUCCESS);
        }
        
        // Create handler processes for each client
        for (int i = 0; i < client_count; i++) {
            int player_id = i + 1;
            client_handlers[i] = sfork();
            if (client_handlers[i] == 0) {
                // Dans le processus fils, on ferme les sockets des autres clients
                for (int j = 0; j < client_count; j++) {
                    if (j != i && client_sockets[j] != -1) {
                        sclose(client_sockets[j]);
                    }
                }
                
                // Et on ferme aussi le pipe d'interception qui est géré par le forwarder
                sclose(intercept_pipe[0]);
                
                client_handler(player_id, client_sockets[i]);
                exit(EXIT_SUCCESS);
            }
        }
        
        // Wait for client handlers to finish
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_handlers[i] > 0) {
                swaitpid(client_handlers[i], NULL, 0);
                client_handlers[i] = -1;
            }
        }
        
        // Kill forwarder
        skill(forwarder_pid, SIGTERM);
        swaitpid(forwarder_pid, NULL, 0);
        
        printf("Game ended\n");
        
        // Close client sockets
        for (int i = 0; i < client_count; i++) {
            if (client_sockets[i] != -1) {
                sclose(client_sockets[i]);
                client_sockets[i] = -1;
            }
        }
    }
    
    // Clean up
    sshmdt(state);
    
    return EXIT_SUCCESS;
}