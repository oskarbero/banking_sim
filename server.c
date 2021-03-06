#include        <sys/time.h>
#include        <sys/types.h>
#include        <sys/socket.h>
#include        <netinet/in.h>
#include        <errno.h>
#include        <semaphore.h>
#include        <pthread.h>
#include        <signal.h>
#include        <stdio.h>
#include        <stdlib.h>
#include        <string.h>
#include        <unistd.h>

#define CLIENT_PORT	35532
#define MAX_CUSTOMERS	20

/*TODO - make sure all the responses are there
 *TODO - clean up the code 
 *TODO - Make a separate version of this, with shared memory -- REread the EC requirements
 */
int serve(char *);
int createAccount(char *);

struct account
{
	char account_name[100];
	int in_use;
	float balance;
};

static pthread_attr_t	user_attr;
static pthread_attr_t	kernel_attr;
static sem_t		actionCycleSemaphore;
static struct account	customers[MAX_CUSTOMERS];
static pthread_mutex_t	c_mutexes[MAX_CUSTOMERS];
static pthread_mutex_t	bank_mutex;
static int		connection_count = 0;

static void
set_iaddr( struct sockaddr_in * sockaddr, long x, unsigned int port )
{
	memset( sockaddr, 0, sizeof(*sockaddr) );
	sockaddr->sin_family = AF_INET;
	sockaddr->sin_port = htons( port );
	sockaddr->sin_addr.s_addr = htonl( x );
}

static char *
ps( unsigned int x, char * s, char * p )
{
	return x == 1 ? s : p;
}


void
periodic_action_handler( int signo, siginfo_t * ignore, void * ignore2 )
{
	if ( signo == SIGALRM )
	{
		sem_post( &actionCycleSemaphore );	
	}
}

void *
periodic_action_cycle_thread( void * ignore )
{
	/* Variables declaration, index, sigaction struct and timer */	
	int			i;
	struct sigaction	action;
	struct itimerval	interval;	

	/* no need for any thread to join on this */
	pthread_detach( pthread_self() );			

	/*initialization of signal handler and timer for the 20 second account print cycle */
	action.sa_flags = SA_SIGINFO | SA_RESTART;
	action.sa_sigaction = periodic_action_handler;
	sigemptyset( &action.sa_mask );
	sigaction( SIGALRM, &action, 0 );
	interval.it_interval.tv_sec = 20;
	interval.it_interval.tv_usec = 0;
	interval.it_value.tv_sec = 20;
	interval.it_value.tv_usec = 0;
	setitimer( ITIMER_REAL, &interval, 0 );			
	for ( ;; ) /* endless loop */
	{
		/* decrement sem count and lock a mutex to go through the bank accounts!*/
		sem_wait( &actionCycleSemaphore );
		pthread_mutex_lock( &bank_mutex);	
		printf( "There %s %d active %s.\nCustomers:\n", ps( connection_count, "is", "are" ),
			connection_count, ps( connection_count, "connection", "connections" ) );
		/* Print the bank! */
		for(i = 0; i < MAX_CUSTOMERS; i++)
		{
			if(customers[i].in_use == 1)
			{
				printf("IN SERVICE: ");
			}
			if(strcmp(customers[i].account_name, "") != 0)
			{
				printf("%s\n", customers[i].account_name);
			}	
		}
		pthread_mutex_unlock( &bank_mutex);
		/* were done with our time slice , give up what we have left */
		sched_yield();	
	}
	return 0;
}

void *
client_session_thread( void * arg )
{
	int			sd;			/*socket descriptor */
	int			cur_customer_index; /* -1 if not loged in , index of account[] when loged in */	
	char			request[2048];
	char			*response;
	char			f_buffer[1024];  /* buffer for float to string conversion */
	char			*buffer;	 /* buffer for parsing the input */	
	/* floats for account balance operations */
	float			withdraw;
	float			deposit;
	float 			customer_balance;
		
	/*store sd and free the dynamically allocated memory for the socked desciptor passed in from pthread_create() */
	sd = *(int *)arg;
	free( arg );					
	
	/*Won't have anyone joining on this thread so we can detach self */
	pthread_detach( pthread_self() );		
	
	/*lock mutex , increase the connection count then unlock */
	pthread_mutex_lock( &bank_mutex );
	++connection_count;				
	pthread_mutex_unlock( &bank_mutex);

	/*make cur customer index -1, this will be changed if a user calls serve (logs in) succesfully
	 *changed back to -1  when he calls end(logs out) */
	cur_customer_index = -1;

	printf( "New Client has connected!\n");
		
	/* while we can keep reading, basically an infinite loop , server quits when we pass in SIGINT from shell */
	while ( read( sd, request, sizeof(request) ) > 0 )
	{
		printf( "\nserver gets input:  %s\n", request );
	
		/* Command parse for the client*/
		if(strncmp(request, "create ", 7) == 0 )
		{
			if(cur_customer_index != -1)
			{
				response = "Logout first to create a new account!";
				write(sd, response, strlen(response)+1);
				response = NULL; /* zero out the buffer */
				continue;
			}	
			/* parse request */
			request[strlen(request)-1] = '\0'; /*consume newline */
			buffer = request+7;   /*begin past the initial command + space*/

			pthread_mutex_lock( &bank_mutex);
				if(createAccount( buffer ) == -1)
				{
					response = "Failed to create a new account`!";
					write(sd, response, strlen(response)+1);
					response = NULL; /* zero out the buffer */
					
				}
			pthread_mutex_unlock( &bank_mutex );	

			response = "New account created!";
			write(sd, response, strlen(response)+1);
			response = NULL; /* zero out the buffer */
			buffer = NULL;	
		}
		else if(strncmp(request, "serve ", 6) == 0)
		{
			/*parse request */
			request[strlen(request)-1] = '\0'; /*eat the \n at the end */
			buffer = request+6;
			/*if cur_customer_index = -1 (this client hasn't loged into anything yet the
			 *lock mutex and check if the customer exists */
			if(cur_customer_index == -1)
			{				
				pthread_mutex_lock( &bank_mutex );
					if((cur_customer_index = serve(buffer)) == -1)
					{
						
						response = "Account currently logged in or doesn't exist";
						write(sd, response, strlen(response)+1);
					 	response = NULL;	
					} 
				pthread_mutex_unlock( &bank_mutex);
			}	
			if(cur_customer_index != -1)
			{
				response = "Login success!";
				write( sd , response , strlen(response) + 1 );
				response = NULL;
			} 
			buffer = NULL; /*zero out the buffer */

		}
		else if(strncmp(request, "quit", 4) == 0)
		{
			/* Check if customer is loged in so we dont deadlock the account before he comes back*/
			if(cur_customer_index != -1)
			{
				pthread_mutex_lock( &c_mutexes[cur_customer_index]);	
				
				customers[cur_customer_index].in_use = 0;
				
				pthread_mutex_unlock( &c_mutexes[cur_customer_index]);
			}
				
			/* we're done here.. close sd, quit thread! */
			close(sd);	
			pthread_exit( 0 );
		}	
		else if(strncmp(request, "end", 3) == 0)
		{	
			if(cur_customer_index == -1)
			{
				response = "Need to log into an account to logout!";
				write(sd, response, strlen(response)+1);
				response = NULL; /* zero out the buffer */
				continue;
			}
			/*lock a mutex and change the in_use variable for currently logged in account to 0 */
			pthread_mutex_lock( &c_mutexes[cur_customer_index]);	
			
			customers[cur_customer_index].in_use = 0;
			
			pthread_mutex_unlock( &c_mutexes[cur_customer_index]);

			/*Client is no longer in use, reset the current index to -1 */
			cur_customer_index = -1;	
			response = "Successfully loged out!";	
			write(sd, response, strlen(response)+1);	
			buffer = NULL;	
			response = NULL;
			
		}
		else if(strncmp(request, "deposit ", 8) == 0)
		{
			if( cur_customer_index == -1)
			{
				response = "Need to log into an account to deposit!";
				write(sd, response, strlen(response)+1);
				response =  NULL; /* zero out the buffer */
				continue;
			}	
			/*parse input from the end of the deposit command, if it cannot be parsed we get 0.00 */	
			deposit = strtof(request+8, NULL);

			/*check for succesfull parse, and that deposit isn't a negative (because then we're withdrawing) */
			if(deposit != 0.0 && deposit >= 0)
			{
				/*lock a mutex and add to the customer's balance */
				pthread_mutex_lock( &c_mutexes[cur_customer_index]);	
				
				customers[cur_customer_index].balance += deposit;
				
				pthread_mutex_unlock( &c_mutexes[cur_customer_index]);
				/* reset the deposit variable to 0 so that there isn't some funny business / just in case */
				deposit = 0;
				response = "Deposit success";	
				write(sd, response, strlen(response)+1);	
				response = NULL;
				
			}
			else	/*if parse failed we got an invalid number as an argument let the heathen know */
			{ 
				write(sd, "Invalid number!", 15);
			}
		}
		else if(strncmp(request, "withdraw ", 9) == 0)
		{	
			if( cur_customer_index == -1)
			{
				response = "Need to log into an account to withdraw!";
				write(sd, response, strlen(response)+1);
				response = NULL; /* zero out the buffer */
				continue;
			}	
			/* parse withdraw to be a float , start checking from the end of "withdraw" command */
			withdraw = strtof(request+9, NULL);
			
			/* check for successful parse (i.e not 0.00) and if withdraw isn't negative 
				(subtracting that would add to account .. if only we could do that ... */
			if(withdraw != 0.0 && withdraw >= 0)
			{	
				/*if parse is good, lock mutex , check if customer has enough money to withdraw
					then subtract deposit amount */
				pthread_mutex_lock( &c_mutexes[cur_customer_index] );	

				if(customers[cur_customer_index].balance >= withdraw)
				{	
					customers[cur_customer_index].balance -= withdraw;
					/*confirm success */
					response = "Withdraw success";	
					write(sd, response, strlen(response)+1);	
					response = NULL;				
				}	
				else /* you poor broke bastard... not enough money #college life */ 
				{
					response = "Not enough money in the account!";
					write(sd, response, strlen(response)+1);
					response = NULL; /* zero out the buffer */
					/* reset the withdraw variable, just in case */
				}
				/*unlock*/
				pthread_mutex_unlock( &c_mutexes[cur_customer_index]);	
			}
			else   /*parse failed, the number was messed up, try again */
			{
				write(sd, "Invalid number!", 15);
			}
			withdraw = 0;
		}
		else if(strncmp(request, "query",5 ) == 0)
		{
			if( cur_customer_index == -1)
			{
				response = "Need to log into an account to check balance!";
				write(sd, response, strlen(response)+1);
				response = NULL; /* zero out the buffer */
				continue;	
			}	
			
			/* lock mutex, store the current logged in account's balance, unlock then write it  to sd */
			pthread_mutex_lock( &c_mutexes[cur_customer_index]);
					
			customer_balance = customers[cur_customer_index].balance;
			
			pthread_mutex_unlock( &c_mutexes[cur_customer_index]);
			
			/*write the amount of money to the buffer whilst converting from float to char * */	
			snprintf(f_buffer,sizeof(f_buffer),"%.2f", customer_balance); 			
			write(sd, f_buffer, strlen(f_buffer) + 1);
			
		}
		else	/*whatever was input doesn't match any commands the server knows, let user know that !*/
		{
			response  = "Unknown Command";
			write(sd, response, strlen(response) +1);
			response = NULL;
		}
		/* write( sd, request, strlen(request) + 1 );*/
	}
	
	/*were done dealing with this client, he quit so close the socket descriptor and subtract connection count */
	close( sd );
	pthread_mutex_lock( &bank_mutex );
	--connection_count;					
	pthread_mutex_unlock( &bank_mutex );
	return 0;
}

void *
session_acceptor_thread( void * ignore )
{
	int			sd;
	int			fd;
	int *			fdptr;
	struct sockaddr_in	addr;
	struct sockaddr_in	senderAddr;
	socklen_t		ic; pthread_t		tid;
	int			on = 1;
	char *			func = "session_acceptor_thread";

	pthread_detach( pthread_self() ); /* no waiting on this thread ! */
	if ( (sd = socket( AF_INET, SOCK_STREAM, 0 )) == -1 ) /* get socket store in sd */
	{
		printf( "socket() failed in %s()\n", func );
		return 0;
	}
	else if ( setsockopt( sd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on) ) == -1 ) /*reserve a port to bind on */
	{
		printf( "setsockopt() failed in %s()\n", func );
		return 0;
	}
	else if ( set_iaddr( &addr, INADDR_ANY, CLIENT_PORT ), errno = 0,
			bind( sd, (const struct sockaddr *)&addr, sizeof(addr) ) == -1 ) /*bind to a port */
	{
		printf( "bind() failed in %s() line %d errno %d\n", func, __LINE__, errno );
		close( sd );
		return 0;
	}
	else if ( listen( sd, 100 ) == -1 )
	{
		printf( "listen() failed in %s() line %d errno %d\n", func, __LINE__, errno ); /*listen to at most 100 connections */
		close( sd );
		return 0;
	}
	else
	{
		ic = sizeof(senderAddr);
		/* accept loop, spawns a new client thread if a succesfull connection occurs , passes in the sd */
		while ( (fd = accept( sd, (struct sockaddr *)&senderAddr, &ic )) != -1 )
		{
			fdptr = (int *)malloc( sizeof(int) );
			*fdptr = fd;					
			if ( pthread_create( &tid, &kernel_attr, client_session_thread, fdptr ) != 0 )
			{
				printf( "pthread_create() failed in %s()\n", func );
				return 0;
			}
			else
			{
				continue;
			}
		}
		/* should really never get here */
		close( sd );
		return 0;
	}
}


int
main( int argc, char ** argv )
{
	pthread_t		tid;
	char *			func = "server main";
	int			i;
	/*
	 * initialize the array of customers before spawning threads
	 */
	for(i = 0; i < MAX_CUSTOMERS; i++)
	{
		strcat(customers[i].account_name, "");	
		customers[i].in_use = -1;
		customers[i].balance = 0;

		/*mutex initialization for all 20 customers!*/
		
		if ( pthread_mutex_init( &c_mutexes[i], NULL ) != 0 )
		{
			printf( "#%i pthread_mutex_init() failed in %s()\n", i , func );
			return 0;
		}	
	}	
	
	/* pthread_attr and mutex  initializations as well as pthread_create for session acceptor anb periodic action handler*/ 
	if ( pthread_attr_init( &user_attr ) != 0 )
	{
		printf( "pthread_attr_init() failed in %s()\n", func );
		return 0;
	}
	else if ( pthread_attr_init( &kernel_attr ) != 0 )
	{
		printf( "pthread_attr_init() failed in %s()\n", func );
		return 0;
	}
	else if ( pthread_attr_setscope( &kernel_attr, PTHREAD_SCOPE_SYSTEM ) != 0 )
	{
		printf( "pthread_attr_setscope() failed in %s() line %d\n", func, __LINE__ );
		return 0;
	}
	else if ( sem_init( &actionCycleSemaphore, 0, 0 ) != 0 )
	{
		printf( "sem_init() failed in %s()\n", func );
		return 0;
	}
	else if ( pthread_mutex_init( &bank_mutex, NULL ) != 0 )
	{
		printf( "pthread_mutex_init() failed in %s()\n", func );
		return 0;
	}	
	else if ( pthread_create( &tid, &kernel_attr, session_acceptor_thread, 0 ) != 0 )
	{
		printf( "pthread_create() failed in %s()\n", func );
		return 0;
	}
	else if ( pthread_create( &tid, &kernel_attr, periodic_action_cycle_thread, 0 ) != 0 )
	{
		printf( "pthread_create() failed in %s()\n", func );
		return 0;
	}
	else  /*don't need main anymore it spawned the threads we want and is now useless, exit the thread */
	{
		printf( "server is ready to receive client connections ...\n" );
		pthread_exit( 0 );
	}
}

/*
 * Serve checks for account existence. If account is found, returns its index
 * in the bank struct array to be used for serve !
 */
int serve(char *acc_name)
{
	int i;
	if(strcmp(acc_name, "") == 0)
	{
		return -1;
	}
	 for(i = 0; i < MAX_CUSTOMERS; i++)
	{
		if(customers[i].account_name != NULL)
		{
			if(strcmp(customers[i].account_name, acc_name) == 0 && customers[i].in_use != 1)
			{
				customers[i].in_use = 1;
				return i;
			}
		}
	}
	
	return -1;
}

/*
 * createAccount (named with java convention for some reason) checks if there is an empty spot
 * for an account to be created and if the account doesnt already exist. If theres a spot and 
 * account doesnt exist yet, it adds the account and returns its index back. Otherwise returns -1.
 */
int createAccount(char *acc_name)
{

	int i;
	if(strlen(acc_name) > 100)
	{
		return -1;
	}
		

	for(i = 0; i < MAX_CUSTOMERS; i++)
	{
		
		if(customers[i].account_name != NULL && strcmp(customers[i].account_name, acc_name) == 0)
		{
			return -1;		
		} 
	}
	/* go through bank struct */
	for(i = 0; i < MAX_CUSTOMERS; i++)
	{
		/*
		 * bank struct initializes account_name to "" so gotta check if it's intact a.k.a still emmpty.
		 */ 	
		if(strcmp(customers[i].account_name, "") == 0)
		{
			strcat(customers[i].account_name, acc_name);
			printf("%s\n", customers[i].account_name);
			return i;
		}
	}	
	return -1;	
}
