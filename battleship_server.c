#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>

#include "linkedlist.h"

// The following mutex locks are used to synchronize threads properly without race conditions
pthread_mutex_t list_lock;	//for ensuring atomicity of operations involving
							//the linked list

pthread_mutex_t thread_lock;	//for ensuring atomicity of operations involving array "threads"
							//and int num_threads

pthread_mutex_t srv_cli_lock;	//for preventing data races involving data passed from main to child threads

pthread_mutex_t log_file_lock;	//for ensuring atomicity of operations when writing to log file

// Circular doubly linked list of players
extern struct node *head;
extern struct node *current;

// Log file to write to and structs necessary to generate accurate + thread safe time stamps
// when writing to the log file
FILE *log_file;
time_t ltime;
struct tm result;
char stime[32];

// Struct used to send data along with created threads
// to client_handler()
typedef struct {
	int *srv;
	int *cli;
	char *client_identifier;
} sock_info;

// SIGINT interrupt routine - should exit safely if encounter sigint
static void sigint_handler(int signal, siginfo_t * t, void *arg)
{
	fprintf(log_file,
		"\nSIGINT signal encountered, exiting gracefully...\n");
	pthread_mutex_unlock(&srv_cli_lock);
	while (head != NULL) {
		deleteNode(head);
		head = head->next;
	}
	fclose(log_file);
	exit(0);
}

// SIGHUP interrupt routine - should exit safely if encounter sighup
static void sighup_handler(int signal, siginfo_t * t, void *arg)
{
	fprintf(log_file,
		"\nSIGHUP signal encountered, exiting gracefully...\n");
	pthread_mutex_unlock(&srv_cli_lock);
	fclose(log_file);
	exit(0);
}

/* Function: getTime()
   Parameters: None
   Returns: char *
   Purpose: Returns a thread-safe and accurate timestamp for use when
			writing to the log file.
*/
char *getTime()
{
	ltime = time(NULL);
	localtime_r(&ltime, &result);
	return asctime_r(&result, stime);
}

/* Function: handle_client(void *package)
   Parameters: void *package
   Returns: void
   Purpose: Target thread function for child threads generated by main process
			to handle incoming client connections. Incoming client connections
			will either have JOIN or BOMB instructions which must be parsed
			and then applied to the game server.
*/
void *handle_client(void *package)
{
	char identifier[5];	// for unique identifier of client
	char x_val[9];		// for 8 byte x value from client
	char y_val[9];		// for 8 byte y value from client
	int x_val_int;		// for int representation of 8 byte x value from client
	int y_val_int;		// for int representation of 8 byte y value from client
	char bbuffer[21];	//4 byte JOIN/BOMB/STAT + two optional 8 byte numbers + null terminator

	// Unpackage sock_info struct containing information from main
	// regarding the server and client sockets
	sock_info *info = package;
	int client = *info->cli;
	strncpy(identifier, info->client_identifier, 4);
	recv(client, bbuffer, sizeof(bbuffer), 0);

	pthread_mutex_unlock(&srv_cli_lock);	//Finished unpacking * package, can now unlock

	// JOIN
	// If first 4 bytes are JOIN, then no 8 byte numbers follow that need to be regarded
	if (strncmp(bbuffer, "JOIN", 4) == 0) {

		pthread_mutex_lock(&list_lock);
		// Get x and y coords sent by the client to bomb
		int j;		//index in bbuffer
		int x = 0;	//index in storval
		for (j = 4; j < 12; j++) {	// first 4 bytes is command (JOIN/BOMB/etc), get next 8 to get x_val
			x_val[x] = bbuffer[j];
			x++;
		}
		x = 0;
		for (j = 12; j < 20; j++) {	// next 8 bytes is y_val
			y_val[x] = bbuffer[j];
		}
		x_val_int = atoi(x_val);
		y_val_int = atoi(y_val);

		// If no solution value was specified by the joining player, assign them
		// x and y solution values
		if (x_val_int == 0 && y_val_int == 0) {
			x_val_int = rand() % (10 + 1 - 1) + 1;	// gen random number between 1 and 10
			y_val_int = rand() % (10 + 1 - 1) + 1;
		}
		insertHead(identifier, x_val_int, y_val_int);

		pthread_mutex_lock(&log_file_lock);
		fprintf(log_file,
			"%s\t: => %s joined the game. His ship is located at x = %d and y = %d.\n",
			getTime(), identifier, x_val_int, y_val_int);
		pthread_mutex_unlock(&log_file_lock);

		if (head->next != head && head->next->next == head) {
			// in this instance we went from 1 player to 2, so game is initiated
			current = head->last;
			pthread_mutex_lock(&log_file_lock);
			fprintf(log_file,
				"%s\t: => The game now has reached the two player minimum. Its status is now active.\n",
				getTime());
			pthread_mutex_unlock(&log_file_lock);
		}
		pthread_mutex_unlock(&list_lock);

	// BOMB
	// If first 4 bytes are BOMB, then two 8 byte numbers follow representing X and Y value to bomb
	// (originally store)
	} else if (strncmp(bbuffer, "BOMB", 4) == 0) {
		pthread_mutex_lock(&list_lock);
		// If we have more than one player
		if (head == NULL) {
			pthread_mutex_lock(&log_file_lock);
			fprintf(log_file, "%s\t:=> %s tried to use the bomb command, but noone has joined the game yet!\n", getTime(), identifier);
			pthread_mutex_unlock(&log_file_lock);
		} else if (head->next == NULL) {
			pthread_mutex_lock(&log_file_lock);
			fprintf(log_file, "%s\t:=> %s tried to use the bomb command, but only one person is in the game! Minimum of 2 people are required.\n", getTime(), identifier);
			pthread_mutex_unlock(&log_file_lock);
		} else if (head->next != head) {
			// Check if its the client's turn, if not, relay this information
			if (strncmp(current->identifier, identifier, 4) == 0) {

				// Get x and y coords sent by the client to bomb
				int j;	//index in bbuffer
				int x = 0;	//index in storval
				for (j = 4; j < 12; j++) {	// first 4 bytes is command (JOIN/BOMB/etc), get next 8 to get x_val
					x_val[x] = bbuffer[j];
					x++;
				}
				x = 0;
				for (j = 12; j < 20; j++) {	// next 8 bytes is y_val
					y_val[x] = bbuffer[j];
				}
				x_val_int = atoi(x_val);
				y_val_int = atoi(y_val);

				// Determine if the shot was a hit and take corresponding action
				int hit = 0;
				if (x_val_int == current->next->x_solution
				    && y_val_int == current->next->y_solution)
					hit = 1;

				fprintf(log_file,
					"%s\t: => It's %s's turn and he bombed %s with values x = %d and y = %d.\n",
					getTime(), current->identifier,
					current->next->identifier, x_val_int,
					y_val_int);

				// If not a hit then simply change current turn to the next person on the chain
				if (hit == 0) {
					pthread_mutex_lock(&log_file_lock);
					fprintf(log_file,
						"%s\t: => %s missed %s. It's now %s's turn.\n",
						getTime(), current->identifier,
						current->next->identifier,
						current->next->identifier);
					pthread_mutex_unlock(&log_file_lock);
					current = current->next;

				// If it is a hit and there is only one person remaining, end the game as they have won.
				// Otherwise remove the hit player from the game and make the next person in the chain
				// after the one who has died the target of the next turn.
				} else if (hit == 1) {
					// If there are only 2 players remaining and the client just hit the other player, the client has won.
					if (current->next->next == current) {
						pthread_mutex_lock
						    (&log_file_lock);
						fprintf(log_file,
							"%s\t: => %s hit %s's ship! %s wins the game as he is the only remaining survivor! Waiting for more challengers...",
							getTime(),
							current->identifier,
							current->next->
							identifier,
							current->identifier);
						pthread_mutex_unlock
						    (&log_file_lock);
					} else {
						pthread_mutex_lock
						    (&log_file_lock);
						fprintf(log_file,
							"%s\t: => %s hit %s's ship! %s is now out of the game. %s is now firing at %s, and it is now %s's turn.\n",
							getTime(),
							current->identifier,
							current->next->
							identifier,
							current->next->
							identifier,
							current->identifier,
							current->next->next->
							identifier,
							current->next->next->
							identifier);
						pthread_mutex_unlock
						    (&log_file_lock);
						current = current->next->next;
						deleteNode(current->last);
					}
				}
				// Client has attempted to 
			} else {
				pthread_mutex_lock(&log_file_lock);
				fprintf(log_file,
					"%s\t:=> %s attempted to take a turn, but it is not his turn.\n",
					getTime(), identifier);
				pthread_mutex_unlock(&log_file_lock);
			}
		} else {
			pthread_mutex_lock(&log_file_lock);
			fprintf(log_file,
				"%s\t:=> %s attempted to take a turn, but there are no other players in the game. At least 2 must join first.\n",
				getTime(), identifier);
		}
		pthread_mutex_unlock(&list_lock);
	}

	close(client);		//finished with this client
	// ---------------------------------------------------------
	free(info);
	return NULL;
}

int main(void)
{

	socklen_t size;
	int srv, cli;

	struct sockaddr_un srvaddr, cliaddr;

	int num_threads = 0;
	pthread_t threads[1024];

	// Seed random with current time for later gen of random numbers
	srand(time(0));

	// Initialize locks that will be used for thread synchronization
	pthread_mutex_init(&list_lock, NULL);
	pthread_mutex_init(&thread_lock, NULL);
	pthread_mutex_init(&srv_cli_lock, NULL);

	// Create a struct for the handling of sigint signals
	// (should call sigint_handler to exit gracefully)
	struct sigaction sa_sigint;
	memset(&sa_sigint, 0, sizeof(sa_sigint));
	sigemptyset(&sa_sigint.sa_mask);
	sa_sigint.sa_sigaction = sigint_handler;
	sa_sigint.sa_flags = SA_SIGINFO;
	sigaction(SIGINT, &sa_sigint, NULL);

	// Create a struct for the handling of sighup signals
	// (should call sighup_handler to print stored numbers
	//  in sorted order, then exit)
	struct sigaction sa_sighup;
	memset(&sa_sighup, 0, sizeof(sa_sighup));
	sigemptyset(&sa_sighup.sa_mask);
	sa_sighup.sa_sigaction = sighup_handler;
	sa_sighup.sa_flags = SA_SIGINFO;
	sigaction(SIGHUP, &sa_sighup, NULL);

	// Open log file for logging
	log_file = fopen("battleship_server.log", "a+");	// a+ (create + append) option will allow appending which is useful in a log file
	if (log_file == NULL) {
		fprintf(stderr, "There was an error opening the log file.\n");
		exit(1);
	}
	// Initialize server socket
	srv = socket(AF_UNIX, SOCK_STREAM, 0);
	// Setup socket struct
	strcpy(srvaddr.sun_path, "./srv_socket");
	srvaddr.sun_family = AF_UNIX;
	size = sizeof(srvaddr);

	// Bind socket to ./srv_socket
	unlink("./srv_socket");	// unlink if still bound to previous server call
	if (bind(srv, (struct sockaddr *)&srvaddr, size) != 0) {
		fprintf(log_file,
			"%s\t: => Error binding server socket. Exiting with non-zero exit status 1.",
			getTime());
		exit(1);
	}
	// Listen on the ./srv_socket socket
	listen(srv, 100);
	fprintf(log_file,
		"%s\t: => Began listening on ./srv_socket for incoming client connections.\n",
		getTime());

	fprintf(stdout, "listening on ./srv_socket\n");

	// Infinite loop, will keep running unless encounter sigint or sighup
	while (1) {

		// Connect with requesting client
		cli = accept(srv, (struct sockaddr *)&cliaddr, &size);

		pthread_mutex_lock(&srv_cli_lock);

		// Package information needed by the handle_client thread
		// worker function into a struct that it can accept as an
		// argument
		sock_info *package = malloc(sizeof *package);
		int srv_copy = srv;
		int cli_copy = cli;
		package->srv = &srv_copy;
		package->cli = &cli_copy;
		package->client_identifier = malloc(5);
		// last 4 chars of cli socket path is the unique client identifier
		package->client_identifier =
		    cliaddr.sun_path + strlen(cliaddr.sun_path) - 4;
		// Create new thread and init it @ handle_client to handle the
		// incoming client
		pthread_mutex_lock(&thread_lock);
		pthread_create(&threads[num_threads], NULL,
			       handle_client, package);
		num_threads++;
		pthread_mutex_unlock(&thread_lock);
	}
}
