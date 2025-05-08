#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <signal.h>
#include "utils_v3.h"
#include "game.h"
#include "pascman.h"

// ANSI color codes for prettier output
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"
#define ANSI_BOLD          "\x1b[1m"

#define DEFAULT_PORT 9090
#define DEFAULT_MAP "./resources/map.txt"

// Global variables for cleanup
pid_t server_pid = -1;
pid_t client1_pid = -1;
pid_t client2_pid = -1;
int pipe_client1[2] = {-1, -1};
int pipe_client2[2] = {-1, -1};
clock_t start_time, end_time;
int p1_moves = 0, p2_moves = 0;

// Forward declarations
void print_mini_map(const char* map_file);
void print_progress_bar(int current, int total, int width);
int count_moves_in_file(const char* filename);
void visualize_move(char move, int player_num);

// Clean up resources
void cleanup() {
    // Close pipes
    if (pipe_client1[0] != -1) close(pipe_client1[0]);
    if (pipe_client1[1] != -1) close(pipe_client1[1]);
    if (pipe_client2[0] != -1) close(pipe_client2[0]);
    if (pipe_client2[1] != -1) close(pipe_client2[1]);
    
    // Kill processes if they're still running
    if (server_pid > 0) kill(server_pid, SIGTERM);
    if (client1_pid > 0) kill(client1_pid, SIGTERM);
    if (client2_pid > 0) kill(client2_pid, SIGTERM);
    
    printf("\n");
}

// Signal handler
void handle_signal(int sig) {
    printf(ANSI_COLOR_RED "\n\nReceived signal %d, cleaning up...\n" ANSI_COLOR_RESET, sig);
    cleanup();
    exit(EXIT_FAILURE);
}

// Displays a mini ASCII version of the map
void print_mini_map(const char* map_file) {
    FILE* map = fopen(map_file, "r");
    if (!map) return;
    
    printf(ANSI_COLOR_YELLOW "\nMap Preview:\n" ANSI_COLOR_RESET);
    printf("┌──────────────────────────────┐\n");
    
    char line[32];
    int line_count = 0;
    while (fgets(line, sizeof(line), map) && line_count < 20) {
        printf("│ ");
        for (int i = 0; i < 28 && line[i] != '\0' && line[i] != '\n'; i++) {
            switch(line[i]) {
                case '#': printf(ANSI_COLOR_BLUE "█" ANSI_COLOR_RESET); break;
                case '.': printf(ANSI_COLOR_YELLOW "·" ANSI_COLOR_RESET); break;
                case '*': printf(ANSI_COLOR_MAGENTA "★" ANSI_COLOR_RESET); break;
                case '@': printf(ANSI_COLOR_GREEN "P" ANSI_COLOR_RESET); break;
                case '!': printf(ANSI_COLOR_RED "P" ANSI_COLOR_RESET); break;
                default:  printf(" "); break;
            }
        }
        printf(" │\n");
        line_count++;
    }
    printf("└──────────────────────────────┘\n");
    fclose(map);
}

// Visual representation of moves
void visualize_move(char move, int player_num) {
    const char* color = (player_num == 1) ? ANSI_COLOR_GREEN : ANSI_COLOR_RED;
    char arrow = ' ';
    
    
    
    printf("%sP%d:%c " ANSI_COLOR_RESET, color, player_num, arrow);
    fflush(stdout);
}

// Displays a visual progress bar
void print_progress_bar(int current, int total, int width) {
    float progress = (float)current / total;
    int bar_width = (int)(progress * width);
    
    printf("\n" ANSI_COLOR_CYAN "[");
    for (int i = 0; i < width; i++) {
        if (i < bar_width) printf("█");
        else printf(" ");
    }
    printf("] %d%%" ANSI_COLOR_RESET, (int)(progress * 100));
    fflush(stdout);
}

// Count moves in a file
int count_moves_in_file(const char* filename) {
    int fd = sopen(filename, O_RDONLY, 0);
    if (fd < 0) return 0;
    
    int count = 0;
    char c;
    while(read(fd, &c, 1) > 0) {
        if (c == '>' || c == '<' || c == 'v' || c == '^') {
            count++;
        }
    }
    
    sclose(fd);
    lseek(fd, 0, SEEK_SET); // Reset file position for later reading
    return count;
}

int main(int argc, char *argv[]) {
    // Register signal handlers for cleanup
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    // Register cleanup function
    atexit(cleanup);
    
    // Parse command line arguments
    int port = DEFAULT_PORT;
    char *map_file = DEFAULT_MAP;
    char *player1_file = NULL;
    char *player2_file = NULL;
    
    if (argc < 5) {
        printf("Usage: %s <port> <map_file> <player1_file> <player2_file>\n", argv[0]);
        printf("Example: %s 9090 ./test/map.txt ./test/joueur1.txt ./test/joueur2.txt\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    port = atoi(argv[1]);
    map_file = argv[2];
    player1_file = argv[3];
    player2_file = argv[4];
    
    // Display header
    printf(ANSI_BOLD "\n╔════════════════════════════════════════╗\n" ANSI_COLOR_RESET);
    printf(ANSI_BOLD "║    PAS-CMAN TEST FRAMEWORK STARTING    ║\n" ANSI_COLOR_RESET);
    printf(ANSI_BOLD "╚════════════════════════════════════════╝\n\n" ANSI_COLOR_RESET);

    printf(ANSI_COLOR_CYAN "• Starting server on port %d using %s\n" ANSI_COLOR_RESET, port, map_file);
    printf(ANSI_COLOR_CYAN "• Player 1 movements: %s\n" ANSI_COLOR_RESET, player1_file);
    printf(ANSI_COLOR_CYAN "• Player 2 movements: %s\n\n" ANSI_COLOR_RESET, player2_file);
    
    // Show map preview
    print_mini_map(map_file);
    
    // Create pipes for client communication
    if (pipe(pipe_client1) == -1 || pipe(pipe_client2) == -1) {
        perror("Failed to create pipes");
        exit(EXIT_FAILURE);
    }
    
    // Count expected moves
    int p1_expected = count_moves_in_file(player1_file);
    int p2_expected = count_moves_in_file(player2_file);
    int total_moves = p1_expected + p2_expected;
    int current_move = 0;
    
    // Start timer
    start_time = clock();
    
    // Start the server
    server_pid = fork();
    if (server_pid == 0) {
        // This is the server process
        char port_str[10];
        sprintf(port_str, "%d", port);
        
        // Close unused pipe ends
        close(pipe_client1[0]);
        close(pipe_client1[1]);
        close(pipe_client2[0]);
        close(pipe_client2[1]);
        
        // Launch server
        execlp("./pas_server", "pas_server", port_str, map_file, NULL);
        
        // If we get here, execlp failed
        perror("Failed to start server");
        exit(EXIT_FAILURE);
    }
    
    printf(ANSI_COLOR_GREEN "✓ Server started (PID: %d)\n" ANSI_COLOR_RESET, server_pid);
    
    // Wait longer for the server to start
    usleep(1000000);  // 1 second
    
    // Start client 1
    client1_pid = fork();
    if (client1_pid == 0) {
        // This is the client 1 process
        // Redirect stdin to read from the pipe
        close(pipe_client1[1]); // Close write end
        dup2(pipe_client1[0], STDIN_FILENO);
        close(pipe_client1[0]);
        
        // Close other pipe ends
        close(pipe_client2[0]);
        close(pipe_client2[1]);
        
        // Convert port to string
        char port_str[10];
        sprintf(port_str, "%d", port);
        
        // Launch client in test mode
        execlp("./pas_client", "pas_client", "localhost", port_str, "-test", NULL);
        
        // If we get here, execlp failed
        perror("Failed to start client 1");
        exit(EXIT_FAILURE);
    }
    
    printf(ANSI_COLOR_GREEN "✓ Client 1 started (PID: %d)\n" ANSI_COLOR_RESET, client1_pid);
    
    // Wait longer before starting the second client
    usleep(1000000);  // 1 second
    
    // Start client 2
    client2_pid = fork();
    if (client2_pid == 0) {
        // This is the client 2 process
        // Redirect stdin to read from the pipe
        close(pipe_client2[1]); // Close write end
        dup2(pipe_client2[0], STDIN_FILENO);
        close(pipe_client2[0]);
        
        // Close other pipe ends
        close(pipe_client1[0]);
        close(pipe_client1[1]);
        
        // Convert port to string
        char port_str[10];
        sprintf(port_str, "%d", port);
        
        // Launch client in test mode
        execlp("./pas_client", "pas_client", "localhost", port_str, "-test", NULL);
        
        // If we get here, execlp failed
        perror("Failed to start client 2");
        exit(EXIT_FAILURE);
    }
    
    printf(ANSI_COLOR_GREEN "✓ Client 2 started (PID: %d)\n" ANSI_COLOR_RESET, client2_pid);
    
    // Parent process feeds movements to clients
    // Close read ends of pipes
    close(pipe_client1[0]);
    close(pipe_client2[0]);
    
    // Wait for clients to initialize and connect
    printf(ANSI_COLOR_YELLOW "Waiting for clients to initialize (3 seconds)...\n" ANSI_COLOR_RESET);
    usleep(3000000);  // 3 seconds
    
    // Open player movement files
    int fd_player1 = sopen(player1_file, O_RDONLY, 0);
    int fd_player2 = sopen(player2_file, O_RDONLY, 0);
    
    printf("\n" ANSI_BOLD "╔═════════════════════════╗\n" ANSI_COLOR_RESET);
    printf(ANSI_BOLD "║    EXECUTING MOVES...    ║\n" ANSI_COLOR_RESET);
    printf(ANSI_BOLD "╚═════════════════════════╝\n\n" ANSI_COLOR_RESET);
    
    printf("Movement sequence: ");
    
    // Use temporary files to store position
    lseek(fd_player1, 0, SEEK_SET);
    lseek(fd_player2, 0, SEEK_SET);
    
    // Variables for tracking move reading
    char move_p1, move_p2;
    int read_p1 = 0, read_p2 = 0;
    bool end_p1 = false, end_p2 = false;
    
    // Alternating player moves
    while (!end_p1 || !end_p2) {
        // Try to make a move with Player 1
        if (!end_p1) {
            do {
                read_p1 = read(fd_player1, &move_p1, 1);
                if (read_p1 <= 0) {
                    end_p1 = true;
                    break;
                }
            } while (move_p1 == ' ' || move_p1 == '\n' || move_p1 == '\r' || move_p1 == '\t');
            
            if (!end_p1 && (move_p1 == '>' || move_p1 == '<' || move_p1 == 'v' || move_p1 == '^')) {
                write(pipe_client1[1], &move_p1, 1);
                visualize_move(move_p1, 1);
                p1_moves++;
                current_move++;
                usleep(500000); // 0.5 seconds between moves
            }
        }
        
        // Try to make a move with Player 2
        if (!end_p2) {
            do {
                read_p2 = read(fd_player2, &move_p2, 1);
                if (read_p2 <= 0) {
                    end_p2 = true;
                    break;
                }
            } while (move_p2 == ' ' || move_p2 == '\n' || move_p2 == '\r' || move_p2 == '\t');
            
            if (!end_p2 && (move_p2 == '>' || move_p2 == '<' || move_p2 == 'v' || move_p2 == '^')) {
                write(pipe_client2[1], &move_p2, 1);
                visualize_move(move_p2, 2);
                p2_moves++;
                current_move++;
                usleep(500000); // 0.5 seconds between moves
            }
        }
        
        // Show progress bar occasionally
        if (current_move % 5 == 0 && total_moves > 0) {
            print_progress_bar(current_move, total_moves, 40);
        }
    }
    
    // End timer
    end_time = clock();
    double test_duration = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    
    // Close files
    sclose(fd_player1);
    sclose(fd_player2);
    
    // Show final progress
    print_progress_bar(total_moves, total_moves, 40);
    
    printf("\n\n");
    printf(ANSI_BOLD "╔════════════════════════════════════════╗\n" ANSI_COLOR_RESET);
    printf(ANSI_BOLD "║       TEST SEQUENCE COMPLETED          ║\n" ANSI_COLOR_RESET);
    printf(ANSI_BOLD "╚════════════════════════════════════════╝\n" ANSI_COLOR_RESET);
    
    // Print test summary
    printf(ANSI_COLOR_YELLOW "\nTest Summary:\n" ANSI_COLOR_RESET);
    printf("┌─────────────────────────────────┐\n");
    printf("│ Total Player 1 Moves: %-10d │\n", p1_moves);
    printf("│ Total Player 2 Moves: %-10d │\n", p2_moves);
    printf("│ Test Duration: %-15.2fs │\n", test_duration);
    printf("└─────────────────────────────────┘\n");
    
    printf(ANSI_COLOR_YELLOW "Waiting 5 seconds to observe results...\n\n" ANSI_COLOR_RESET);
    sleep(5);  // Wait 5 seconds before cleanup
    
    // Cleanup is handled by atexit
    return 0;
}