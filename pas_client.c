#include "utils_v3.h"
#include "game.h"
#include "pascman.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdbool.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 74912
#define UI_PATH "./pas-cman-ipl"
#define BUFFER_SIZE 1024

// Global variables for cleanup
int server_socket = -1;
int ui_to_client_pipe[2] = {-1, -1};
int client_to_ui_pipe[2] = {-1, -1};
pid_t ui_pid = -1;
bool running = true;

void cleanup() {
    // Close socket
    if (server_socket != -1) {
        sclose(server_socket);
        server_socket = -1;
    }
    
    // Close pipes
    if (ui_to_client_pipe[0] != -1) {
        sclose(ui_to_client_pipe[0]);
        ui_to_client_pipe[0] = -1;
    }
    if (ui_to_client_pipe[1] != -1) {
        sclose(ui_to_client_pipe[1]);
        ui_to_client_pipe[1] = -1;
    }
    
    if (client_to_ui_pipe[0] != -1) {
        sclose(client_to_ui_pipe[0]);
        client_to_ui_pipe[0] = -1;
    }
    if (client_to_ui_pipe[1] != -1) {
        sclose(client_to_ui_pipe[1]);
        client_to_ui_pipe[1] = -1;
    }
    
    // Terminate UI process
    if (ui_pid > 0) {
        skill(ui_pid, SIGTERM);
        swaitpid(ui_pid, NULL, 0);
        ui_pid = -1;
    }
    
    printf("Client cleaned up and exiting\n");
}

// Signal handler for SIGINT
void sigint_handler(int sig) {
    if (sig == SIGINT) {
        printf("SIGINT received, client will stop\n");
        running = false;
    }
}

/**
 * Handle the game over state transition
 * 
 * This function properly formats the game over message for the UI
 * based on whether this player won or lost.
 */
bool handle_game_over(union Message *msg, int player_id, int client_to_ui_fd, struct pollfd *server_poll_fd) {
    if (msg->msgt != GAME_OVER) {
        return false;
    }
    
    // Get the winner ID from the message
    int winner_id = msg->game_over.winner;
    
    // Display message in terminal with clear visual separation
    printf("\n==================================\n");
    printf("GAME OVER! Player %d wins!\n", winner_id);
    
    if (player_id == winner_id) {
        printf("*** YOU WIN! ***\n");
    } else {
        printf("*** YOU LOSE! ***\n");
    }
    printf("==================================\n\n");
    
    // Create message for UI
    union Message ui_msg;
    memset(&ui_msg, 0, sizeof(union Message));
    ui_msg.msgt = GAME_OVER;
    ui_msg.game_over.msgt = GAME_OVER;
    ui_msg.game_over.winner = winner_id;
    
    printf("Sending GAME_OVER to UI: player_id=%d, winner=%d\n", 
           player_id, winner_id);
    
    // Send message to UI
    if (write(client_to_ui_fd, &ui_msg, sizeof(union Message)) <= 0) {
        perror("Failed to send GAME_OVER to UI");
    }
    
    // Ensure UI process gets the message by flushing
    fsync(client_to_ui_fd);
    
    printf("Game over screen displayed. Press ENTER in game window to exit.\n");
    
    // Stop listening to server but keep UI connection active
    server_poll_fd->fd = -1;
    
    // Important: nous ne terminons pas le programme ici
    // nous laissons l'interface afficher l'écran de fin
    // et attendons que l'utilisateur appuie sur une touche
    return true;
}

int main(int argc, char *argv[]) {
    printf("Starting PAS-CMAN client...\n");
    
    char *server_ip = SERVER_IP;
    int server_port = SERVER_PORT;
    
    // Parse command line arguments if provided
    if (argc >= 2) {
        server_ip = argv[1];
    }
    if (argc >= 3) {
        server_port = atoi(argv[2]);
    }
    
    // Set up signal handler
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    
    // Register cleanup function
    atexit(cleanup);
    
    // Create pipes for UI communication
    spipe(ui_to_client_pipe);  // UI writes, client reads
    spipe(client_to_ui_pipe);  // Client writes, UI reads
    
    // Check if UI executable exists before forking
    if (access(UI_PATH, X_OK) != 0) {
        perror("UI executable not found or not executable");
        printf("Please make sure %s exists and has execute permissions\n", UI_PATH);
        cleanup();
        exit(EXIT_FAILURE);
    }
    
    // Create UI process
    ui_pid = sfork();
    if (ui_pid == 0) {
        // Child process (UI)
        
        // Set up stdin/stdout redirection
        sclose(ui_to_client_pipe[0]);  // Close read end
        sclose(client_to_ui_pipe[1]);  // Close write end
        
        // Redirect stdout to write to ui_to_client_pipe
        sdup2(ui_to_client_pipe[1], STDOUT_FILENO);
        sclose(ui_to_client_pipe[1]);
        
        // Redirect stdin to read from client_to_ui_pipe
        sdup2(client_to_ui_pipe[0], STDIN_FILENO);
        sclose(client_to_ui_pipe[0]);
        
        // Execute UI program avec execv au lieu de execl pour plus de fiabilité
        char *args[] = {UI_PATH, NULL};
        execv(UI_PATH, args);
        
        // If execv returns, there was an error
        perror("UI execution failed");
        printf("Failed to execute %s - make sure it exists and has execute permissions\n", UI_PATH);
        exit(EXIT_FAILURE);
    }
    
    // Parent process (client)
    sclose(ui_to_client_pipe[1]);  // Close write end
    sclose(client_to_ui_pipe[0]);  // Close read end
    
    // Add delay before connecting to server
    printf("Initializing UI, waiting 1 second before connecting to server...\n");
    usleep(1000000);  // 1 second delay
    
    // Connect to server
    printf("Connecting to server at %s:%d...\n", server_ip, server_port);
    server_socket = ssocket();
    sconnect(server_ip, server_port, server_socket);
    printf("Connected to server\n");
    
    // Set up polling for server and UI
    struct pollfd poll_fds[2];
    poll_fds[0].fd = server_socket;
    poll_fds[0].events = POLLIN;
    poll_fds[1].fd = ui_to_client_pipe[0];
    poll_fds[1].events = POLLIN;
    
    // Main client loop
    bool game_over = false;
    int player_id = 0;
    int message_count = 0;
    
    while (running) {
        int poll_result = spoll(poll_fds, 2, 500);  // 500ms timeout pour vérifier running régulièrement
        
        if (poll_result > 0) {
            // Check for data from server
            if (poll_fds[0].revents & POLLIN) {
                union Message msg;
                memset(&msg, 0, sizeof(union Message));
                
                // Try to read the message from server
                struct sigaction old_action;
                // Temporarily ignore SIGPIPE
                struct sigaction temp_action;
                temp_action.sa_handler = SIG_IGN;
                sigemptyset(&temp_action.sa_mask);
                temp_action.sa_flags = 0;
                sigaction(SIGPIPE, &temp_action, &old_action);
                
                ssize_t bytes_read = read(server_socket, &msg, sizeof(union Message));
                
                // Restore original signal handling
                sigaction(SIGPIPE, &old_action, NULL);
                
                if (bytes_read <= 0) {
                    // Handle connection reset specially
                    if (errno == ECONNRESET || errno == 0) {
                        printf("Connection reset or closed by server - assuming game is over\n");
                        
                        // If we didn't already see a GAME_OVER message, create one
                        if (!game_over && poll_fds[0].fd != -1) {
                            printf("Creating synthetic game over message\n");
                            
                            // Create a synthetic game over message
                            union Message synthetic_msg;
                            memset(&synthetic_msg, 0, sizeof(union Message));
                            synthetic_msg.msgt = GAME_OVER;
                            synthetic_msg.game_over.msgt = GAME_OVER;
                            
                            // If player_id is set, we might be the winner
                            if (player_id > 0) {
                                synthetic_msg.game_over.winner = player_id;
                                printf("Game ended unexpectedly - Assuming Player %d won\n", player_id);
                            } else {
                                // Default to player 1 if we don't know our player ID
                                synthetic_msg.game_over.winner = 1;
                                printf("Game ended unexpectedly - Assuming Player 1 won\n");
                            }
                            
                            // Use our helper function to handle the game over state
                            game_over = handle_game_over(&synthetic_msg, player_id, client_to_ui_pipe[1], &poll_fds[0]);
                            continue;
                        }
                    }
                    
                    printf("Server disconnected (errno=%d)\n", errno);
                    running = false;
                    break;
                }
                
                // Process received message
                message_count++;
                printf("Client received message #%d of type %d\n", message_count, msg.msgt);
                
                // Forward message to UI (except for GAME_OVER which we'll handle specially)
                if (msg.msgt != GAME_OVER) {
                    // Débug pour messages SPAWN
                    if (msg.msgt == SPAWN) {
                        printf("Forwarding SPAWN: id=%u, item=%d, pos=(%u,%u)\n", 
                               msg.spawn.id, msg.spawn.item, msg.spawn.pos.x, msg.spawn.pos.y);
                    }
                    
                    // Assurer que les messages SPAWN sont correctement envoyés à l'UI
                    if (swrite(client_to_ui_pipe[1], &msg, sizeof(union Message)) < 0) {
                        perror("Failed to forward message to UI");
                    } else {
                        fsync(client_to_ui_pipe[1]); // S'assurer que le message est envoyé immédiatement
                    }
                }
                
                // Special handling for certain message types
                switch (msg.msgt) {
                    case REGISTRATION:
                        player_id = msg.registration.player;
                        printf("Registered as Player %d\n", player_id);
                        break;
                        
                    case SPAWN:
                        printf("Received SPAWN: id=%u, item=%d, pos=(%u,%u)\n", 
                               msg.spawn.id, msg.spawn.item, msg.spawn.pos.x, msg.spawn.pos.y);
                        break;
                        
                    case GAME_OVER:
                        // Special handling for game over
                        game_over = handle_game_over(&msg, player_id, client_to_ui_pipe[1], &poll_fds[0]);
                        break;
                        
                    default:
                        break;
                }
            }
            
            // Check for data from UI
            if (poll_fds[1].revents & POLLIN) {
                enum Direction dir;
                ssize_t bytes_read = sread(ui_to_client_pipe[0], &dir, sizeof(enum Direction));
                
                if (bytes_read <= 0) {
                    printf("UI disconnected\n");
                    running = false;
                    break;
                }
                
                // If we're in game over state, any input means "exit"
                if (game_over || poll_fds[0].fd == -1) {
                    printf("User pressed key after game over, exiting...\n");
                    running = false;
                    break;
                }
                
                // Otherwise forward direction to server
                printf("Sending direction %d to server\n", dir);
                if (swrite(server_socket, &dir, sizeof(enum Direction)) <= 0) {
                    perror("Failed to send direction to server");
                }
            }
            
            // Check if either endpoint has closed with POLLHUP or POLLERR
            if ((poll_fds[0].revents & (POLLHUP | POLLERR)) || 
                (poll_fds[1].revents & (POLLHUP | POLLERR))) {
                printf("Connection closed\n");
                running = false;
                break;
            }
        }
        // Si poll timeout, juste continuer pour vérifier running périodiquement
    }
    
    printf("Client shutting down\n");
    return EXIT_SUCCESS;
}