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
#include <errno.h>

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

// Server phases
typedef enum {
    PHASE_IDLE,           // Server is waiting for first player to connect
    PHASE_REGISTRATION,   // Registration phase (after first player connected)
    PHASE_GAME            // Game in progress
} ServerPhase;

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
bool shutdown_requested = false; // Flag to track if SIGINT was received
bool registration_timed_out = false; // Flag to track registration timeout
ServerPhase current_phase = PHASE_IDLE; // Current server phase
char *g_map_file = DEFAULT_MAP_FILE;

void cleanup() {
    // Block all signals during cleanup to avoid interruptions
    sigset_t mask_all, prev_mask;
    sigfillset(&mask_all);
    sigprocmask(SIG_SETMASK, &mask_all, &prev_mask);
    
    // Kill client handlers
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_handlers[i] > 0) {
            skill(client_handlers[i], SIGTERM);
            // Use waitpid with error handling
            int status;
            pid_t result;
            int wait_attempts = 0;
            int max_wait_attempts = 5;
            
            do {
                result = waitpid(client_handlers[i], &status, WNOHANG);
                if (result == 0) {
                    // Process still running, give it a moment
                    usleep(50000); // 50ms delay
                    wait_attempts++;
                    if (wait_attempts >= max_wait_attempts) {
                        // If we've waited too long, forcibly kill the process
                        printf("Client handler %d not responding, sending SIGKILL\n", i+1);
                        kill(client_handlers[i], SIGKILL);
                    }
                }
            } while (result == 0 && wait_attempts < max_wait_attempts*2);
            
            client_handlers[i] = -1;
        }
    }
      // Kill broadcaster
    if (broadcaster_pid > 0) {
        skill(broadcaster_pid, SIGTERM);
        // Use waitpid with error handling
        int status;
        pid_t result;
        int wait_attempts = 0;
        int max_wait_attempts = 5;
        
        do {
            result = waitpid(broadcaster_pid, &status, WNOHANG);
            if (result == 0) {
                // Process still running, give it a moment
                usleep(50000); // 50ms delay
                wait_attempts++;
                if (wait_attempts >= max_wait_attempts) {
                    // If we've waited too long, forcibly kill the process
                    printf("Broadcaster not responding, sending SIGKILL\n");
                    kill(broadcaster_pid, SIGKILL);
                }
            }
        } while (result == 0 && wait_attempts < max_wait_attempts*2);
        
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
    
    // Close server socket - try multiple times if needed
    if (sockfd != -1) {
        int close_attempts = 0;
        while (close_attempts < 3) {
            if (close(sockfd) == 0 || errno != EBADF) {
                break;  // Successfully closed or different error
            }
            usleep(50000);  // 50ms delay between attempts
            close_attempts++;
        }
        sockfd = -1;
    }
    
    // Clean up shared memory
    if (shm_id != -1) {
        sshmdelete(shm_id);
        shm_id = -1;
    }        // Clean up semaphores - try multiple times if needed
    if (sem_id != -1) {
        int sem_attempts = 0;
        while (sem_attempts < 3) {
            errno = 0;
            sem_delete(sem_id);
            if (errno == 0 || errno != EINVAL) {
                break;  // Successfully deleted or different error
            }
            usleep(50000);  // 50ms delay between attempts
            sem_attempts++;
        }
        sem_id = -1;
    }
    
    // Set this flag to prevent double cleanup in some error cases
    static int already_cleaned_up = 0;
    already_cleaned_up = 1;
    
    // Restore previous signal mask
    sigprocmask(SIG_SETMASK, &prev_mask, NULL);
    
    printf("Server cleaned up and exiting\n");
}

// Empty handler for SIGUSR1 (used to unblock waitpid)
void sigusr1_handler(int sig) {
    // Do nothing, this is just to wake up processes blocked in system calls
}

// Signal handler for SIGINT and SIGALRM
void sigint_handler(int sig) {
    if (sig == SIGINT) {
        // If we already requested shutdown, don't show the message again
        if (shutdown_requested) {
            return;
        }

        const char *phase_name;
        switch (current_phase) {
            case PHASE_IDLE:
                phase_name = "idle";
                break;
            case PHASE_REGISTRATION:
                phase_name = "registration";
                break;
            case PHASE_GAME:
                phase_name = "game";
                break;
            default:
                phase_name = "unknown";
        }
          // Set the shutdown flag
        shutdown_requested = true;
        
        // Only set running=false immediately if we're in IDLE phase
        if (current_phase == PHASE_IDLE) {
            printf("SIGINT received during %s phase, server will stop immediately\n", phase_name);
            running = false;
        } else {
            printf("SIGINT received during %s phase, server will stop after current phase completes\n", phase_name);
            // For other phases, we'll check the shutdown_requested flag 
            // when transitioning back to IDLE
              
            // For game phase, we should let the game complete
            if (current_phase == PHASE_GAME) {
                printf("Server will continue running until the game completes naturally.\n");
                printf("Clients will continue to operate normally.\n");
            }
        }
    } else if (sig == SIGALRM) {
        printf("Registration timeout: Not enough players connected within 30 seconds\n");
        registration_timed_out = true; // Set the flag when the alarm occurs
    }
}

// Custom poll function that handles EINTR (interrupted by signal)
int poll_with_retry(struct pollfd *fds, nfds_t nfds, int timeout) {
    int ret;
    while ((ret = poll(fds, nfds, timeout)) == -1 && errno == EINTR) {
        // If interrupted by a signal, check if it was SIGALRM during registration
        if (registration_timed_out) {
            // Return 0 to indicate timeout, which will cause the main loop to process the timeout
            return 0;
        }
        
        printf("Poll interrupted by signal, retrying...\n");
        continue;
    }
    
    // For other errors, use the standard error checking
    checkNeg(ret, "poll failure");
    return ret;
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
        int poll_result = poll_with_retry(&poll_fd, 1, -1);
        
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
        // Wait for direction input from client - handle EINTR specially
        ssize_t bytes_read;
        do {
            bytes_read = read(client_socket, direction_buffer, sizeof(direction_buffer));
            if (bytes_read < 0 && errno == EINTR) {
                // If interrupted by signal, just try again
                printf("Client %d read was interrupted by signal, retrying...\n", client_num);
                continue;
            }
        } while (bytes_read < 0 && errno == EINTR);
        
        // Check for other errors
        if (bytes_read <= 0) {
            // Client disconnected or error
            if (bytes_read < 0) {
                perror("Client read error");
            }
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
    
    // Set up SIGUSR1 handler
    struct sigaction sa_usr1;
    sa_usr1.sa_handler = sigusr1_handler;
    sigemptyset(&sa_usr1.sa_mask);
    sa_usr1.sa_flags = 0;
    sigaction(SIGUSR1, &sa_usr1, NULL);
    
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
    spipe(broadcast_pipe);    // Create and set up server socket
    sockfd = ssocket();
    
    // Set SO_REUSEADDR to allow binding to a recently closed socket
    int option_value = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &option_value, sizeof(int)) < 0) {
        perror("Error setting socket options");
    }
      
    // Try to bind with timeout in case previous instance didn't fully release the port
    int bind_attempts = 0;
    int max_attempts = 5;
    while (bind_attempts < max_attempts) {
        // Try to use sbind from utils_v3
        int bind_result = sbind(port, sockfd);
        
        if (bind_result == 0) {
            break; // Successfully bound
        }
        
        // If binding failed, check if it was an "Address already in use" error
        if (errno != EADDRINUSE) {
            // This is not the error we're trying to handle, so report it and exit
            fprintf(stderr, "Failed to bind to port %d: %s\n", port, strerror(errno));
            exit(EXIT_FAILURE);
        }
        
        // If binding failed with EADDRINUSE, wait a bit and try again
        printf("Port %d is in use (attempt %d/%d), waiting 2 seconds...\n", 
               port, bind_attempts + 1, max_attempts);
        sleep(2); // Wait longer between attempts
        bind_attempts++;
    }
    
    if (bind_attempts == max_attempts) {
        printf("Failed to bind to port %d after %d attempts. Try killing any existing server process.\n", 
               port, max_attempts);
        exit(EXIT_FAILURE);
    }
    
    slisten(sockfd, BACKLOG);
    
    printf("Server started on port %d, waiting for clients...\n", port);
    
    // Create a pipe for intercepting and forwarding broadcast messages
    int intercept_pipe[2];
    spipe(intercept_pipe);
      // Start broadcaster process - now it will write to intercept_pipe
    broadcaster_pid = sfork();
    if (broadcaster_pid == 0) {        // Child process (broadcaster) - ignore SIGINT to avoid multiple handlers
        struct sigaction sa_ignore;
        sa_ignore.sa_handler = SIG_IGN; // Ignore SIGINT
        sigemptyset(&sa_ignore.sa_mask);
        sa_ignore.sa_flags = 0;
        sigaction(SIGINT, &sa_ignore, NULL);
        
        // Set up SIGUSR1 handler for broadcaster
        struct sigaction sa_usr1;
        sa_usr1.sa_handler = sigusr1_handler;
        sigemptyset(&sa_usr1.sa_mask);
        sa_usr1.sa_flags = 0;
        sigaction(SIGUSR1, &sa_usr1, NULL);
        
        sclose(broadcast_pipe[1]);  // Close write end
        sclose(intercept_pipe[0]);  // Close read end of intercept pipe
        
        // Set up polling for the broadcast pipe
        struct pollfd poll_fd;
        poll_fd.fd = broadcast_pipe[0];
        poll_fd.events = POLLIN;
        
        while (1) {
            // Poll for incoming messages
            int poll_result = poll_with_retry(&poll_fd, 1, -1);
              if (poll_result > 0 && (poll_fd.revents & POLLIN)) {
                // Nous allons lire le message complet
                union Message msg;
                memset(&msg, 0, sizeof(union Message));
                
                // Handle EINTR for read operations
                ssize_t bytes_read;
                do {
                    bytes_read = read(broadcast_pipe[0], &msg, sizeof(union Message));
                    if (bytes_read < 0 && errno == EINTR) {
                        // If interrupted by signal, just try again
                        printf("Broadcaster read was interrupted by signal, retrying...\n");
                        continue;
                    }
                } while (bytes_read < 0 && errno == EINTR);
                
                if (bytes_read <= 0) {
                    if (bytes_read < 0) {
                        perror("Broadcaster read error");
                    }
                    break;  // Pipe closed or error
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
        // At the start of each main loop, check if we need to exit (we're in IDLE phase)
        if (shutdown_requested && current_phase == PHASE_IDLE) {
            printf("Shutdown requested while server is idle, exiting\n");
            running = false;
            break;
        }
        
        printf("Waiting for players to connect...\n");
        
        // Game registration phase (30 seconds timeout)
        int client_sockets[MAX_CLIENTS] = {-1, -1};
        int client_count = 0;
        
        // Reset the timeout flag before starting a new registration phase
        registration_timed_out = false;
        
        // Set the server phase to IDLE (waiting for first player)
        current_phase = PHASE_IDLE;
        
        // Set a 30-second alarm for registration phase, but only after first player connects
        alarm(0); // Clear any previous alarm
          // Accept client connections
        while (client_count < MAX_CLIENTS && running && !registration_timed_out) {
            // We'll use poll with a timeout to check running flag periodically
            struct pollfd poll_fd;
            poll_fd.fd = sockfd;
            poll_fd.events = POLLIN;
            
            // Use a short poll timeout to check the running flag regularly
            int poll_result = poll_with_retry(&poll_fd, 1, 1000); // 1 second timeout
            
            // Check if we need to stop due to SIGINT when we're in IDLE phase
            // We only set running=false immediately if in IDLE phase with no players
            if (!running && client_count == 0) {
                printf("Shutdown requested while server is idle with no clients, exiting immediately\n");
                break;
            }
            
            // Check if registration timed out
            if (registration_timed_out) {
                printf("Registration phase timed out, disconnecting players and restarting\n");
                break; // Exit the connection loop to handle timeout
            }
            
            // If no activity on the socket, continue polling
            if (poll_result == 0) {
                continue;
            }
            
            // Accept connection if available
            if (poll_result > 0 && (poll_fd.revents & POLLIN)) {
                int client_socket = saccept(sockfd);
                
                client_sockets[client_count] = client_socket;
                client_count++;
                
                printf("Client %d connected\n", client_count);
                
                // Start the 30-second timer after first player connects
                if (client_count == 1) {
                    // Change phase to REGISTRATION after first player connects
                    current_phase = PHASE_REGISTRATION;
                    printf("First player connected. Registration phase started: %d seconds timeout\n", REGISTRATION_TIMEOUT);
                    alarm(REGISTRATION_TIMEOUT);
                }
                
                // If we've got all required clients, cancel the alarm
                if (client_count == MAX_CLIENTS) {
                    alarm(0);  // Cancel the alarm
                    printf("All players connected, registration phase complete\n");
                }
            }
        }
        
        // Cancel alarm in case we're exiting the loop for another reason
        alarm(0);
        
        // If we didn't get enough clients or received SIGALRM, disconnect and restart
        if (client_count < MAX_CLIENTS || registration_timed_out) {
            printf("Not enough players connected. Disconnecting players and restarting registration.\n");
            for (int i = 0; i < client_count; i++) {
                sclose(client_sockets[i]);
                client_sockets[i] = -1;
            }
              // Reset back to IDLE phase
            current_phase = PHASE_IDLE;
            
            // Check if shutdown was requested during any previous phase
            if (shutdown_requested) {
                printf("Shutdown requested and returning to IDLE phase, exiting server\n");
                running = false; // Only set running=false when back in IDLE phase
                break;
            }
            
            continue;  // Restart registration phase
        }
        
        // Change to GAME phase
        current_phase = PHASE_GAME;
        printf("Entering GAME phase\n");
        
        // Reset game state for new game
        reset_gamestate(state);
        
        // Fork a process to handle the interception and forwarding of messages to clients
        pid_t forwarder_pid = sfork();
          if (forwarder_pid == 0) {            // Child process (forwarder) - ignore SIGINT to avoid multiple handlers
            struct sigaction sa_ignore;
            sa_ignore.sa_handler = SIG_IGN; // Ignore SIGINT
            sigemptyset(&sa_ignore.sa_mask);
            sa_ignore.sa_flags = 0;
            sigaction(SIGINT, &sa_ignore, NULL);
            
            // Set up SIGUSR1 handler for forwarder
            struct sigaction sa_usr1;
            sa_usr1.sa_handler = sigusr1_handler;
            sigemptyset(&sa_usr1.sa_mask);
            sa_usr1.sa_flags = 0;
            sigaction(SIGUSR1, &sa_usr1, NULL);
            
            printf("Message forwarder started\n");
            
            // Set up polling for interceptor pipe
            struct pollfd poll_fd;
            poll_fd.fd = intercept_pipe[0];
            poll_fd.events = POLLIN;
            
            while (1) {                int poll_result = poll_with_retry(&poll_fd, 1, -1);
                
                if (poll_result > 0 && (poll_fd.revents & POLLIN)) {
                    union Message msg;
                    memset(&msg, 0, sizeof(union Message));
                    
                    // Handle EINTR for read operations
                    ssize_t bytes_read;
                    do {
                        bytes_read = read(intercept_pipe[0], &msg, sizeof(union Message));
                        if (bytes_read < 0 && errno == EINTR) {
                            // If interrupted by signal, just try again
                            printf("Forwarder read was interrupted by signal, retrying...\n");
                            continue;
                        }
                    } while (bytes_read < 0 && errno == EINTR);
                    
                    if (bytes_read <= 0) {
                        if (bytes_read < 0) {
                            perror("Forwarder read error");
                        }
                        break;  // Pipe closed or error
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
            int player_id = i + 1;            client_handlers[i] = sfork();
            if (client_handlers[i] == 0) {                // Child process - ignore SIGINT to avoid multiple handlers
                struct sigaction sa_ignore;
                sa_ignore.sa_handler = SIG_IGN; // Ignore SIGINT
                sigemptyset(&sa_ignore.sa_mask);
                sa_ignore.sa_flags = 0;
                sigaction(SIGINT, &sa_ignore, NULL);
                
                // Set up SIGUSR1 handler for client handler
                struct sigaction sa_usr1;
                sa_usr1.sa_handler = sigusr1_handler;
                sigemptyset(&sa_usr1.sa_mask);
                sa_usr1.sa_flags = 0;
                sigaction(SIGUSR1, &sa_usr1, NULL);

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
        }          // Wait for client handlers to finish - this will be the game phase
        printf("Game is now running. It will continue until completion even if shutdown is requested.\n");
        
        // Use a flag to track if the game ended naturally with a "game over"
        // or if it was interrupted by SIGINT
        bool game_interrupted = false;
        
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_handlers[i] > 0) {
                // Use waitpid directly with error handling for EINTR
                int status;
                pid_t result;
                do {
                    result = waitpid(client_handlers[i], &status, 0);
                    if (result == -1 && errno == EINTR) {
                        if (shutdown_requested) {
                            // SIGINT was received, but we should let the game continue
                            printf("Waitpid was interrupted by SIGINT, but we'll keep waiting for game to complete...\n");
                            game_interrupted = true;
                        } else {
                            printf("Waitpid was interrupted by signal, retrying...\n");
                        }
                    }
                } while (result == -1 && errno == EINTR);
                
                // Only report errors that aren't caused by interrupted system call
                if (result == -1 && errno != EINTR) {
                    perror("Error waiting for client handler");
                }
                
                // Check exit status of client handler to verify if it exited normally or was terminated
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    printf("Client handler %d exited normally\n", i+1);
                } else {
                    printf("Client handler %d terminated abnormally\n", i+1);
                    game_interrupted = true;
                }
                
                client_handlers[i] = -1;
            }
        }
        
        if (!game_interrupted) {
            printf("Game has ended naturally with a game over.\n");
        } else {
            printf("Game was interrupted by SIGINT but handled gracefully.\n");
        }
        
        // Kill forwarder
        skill(forwarder_pid, SIGTERM);
        // Use waitpid directly with error handling for EINTR
        int fw_status;
        pid_t fw_result;
        do {
            fw_result = waitpid(forwarder_pid, &fw_status, 0);
            if (fw_result == -1 && errno == EINTR) {
                printf("Waitpid for forwarder was interrupted by signal, retrying...\n");
            }
        } while (fw_result == -1 && errno == EINTR);
        
        // Close client sockets
        for (int i = 0; i < client_count; i++) {
            if (client_sockets[i] != -1) {
                sclose(client_sockets[i]);
                client_sockets[i] = -1;
            }
        }
          // Set the server phase back to IDLE after game completes
        current_phase = PHASE_IDLE;
        
        // After a game is complete, check if shutdown was requested during any phase
        if (shutdown_requested) {
            printf("Shutdown requested and game has ended, returning to IDLE phase, exiting server\n");
            running = false; // Only set running=false when back in IDLE phase
            break;
        }
    }
    
    // Clean up
    sshmdt(state);
    
    return EXIT_SUCCESS;
}