#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <assert.h>
#include <pthread.h>

#include "babble_server.h"
#include "babble_types.h"
#include "babble_utils.h"
#include "babble_communication.h"


#define BUFFER_SIZE 3


pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER; //Mutex variable
pthread_cond_t empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t full = PTHREAD_COND_INITIALIZER;


int count_thread=0;
int c_index= 0;   // Index for communicator
int e_index = 0; // Index for the executor 


// Data Structure to be passed
typedef struct arguments
 {
    int newsockfd;
    unsigned long client_key;
    char* client_name;   

} exec_args;

command_t* BUFFER; /*Buffer to store the command received from the communicator thread and
 being read by the execution thread*/

static void display_help(char *exec)
{
    printf("Usage: %s -p port_number\n", exec);
}


static int parse_command(char* str, command_t *cmd)
{
    /* start by cleaning the input */
    str_clean(str);
    
    /* get command id */
    cmd->cid=str_to_command(str, &cmd->answer_exp);

    /* initialize other fields */
    cmd->answer.size=-1;
    cmd->answer.aset=NULL;

    switch(cmd->cid){
    case LOGIN:
        if(str_to_payload(str, cmd->msg, BABBLE_ID_SIZE)){
            fprintf(stderr,"Error -- invalid LOGIN -> %s\n", str);
            return -1;
        }
        break;
    case PUBLISH:
        if(str_to_payload(str, cmd->msg, BABBLE_SIZE)){
            fprintf(stderr,"Warning -- invalid PUBLISH -> %s\n", str);
            return -1;
        }
        break;
    case FOLLOW:
        if(str_to_payload(str, cmd->msg, BABBLE_ID_SIZE)){
            fprintf(stderr,"Warning -- invalid FOLLOW -> %s\n", str);
            return -1;
        }
        break;
    case TIMELINE:
        cmd->msg[0]='\0';
        break;
    case FOLLOW_COUNT:
        cmd->msg[0]='\0';
        break;
    case RDV:
        cmd->msg[0]='\0';
        break;    
    default:
        fprintf(stderr,"Error -- invalid client command -> %s\n", str);
        return -1;
    }

    return 0;
}


static int process_command(command_t *cmd)
{
    int res=0;

    switch(cmd->cid){
    case LOGIN:
        res = run_login_command(cmd);
        break;
    case PUBLISH:
        res = run_publish_command(cmd);
        break;
    case FOLLOW:
        res = run_follow_command(cmd);
        break;
    case TIMELINE:
        res = run_timeline_command(cmd);
        break;
    case FOLLOW_COUNT:
        res = run_fcount_command(cmd);
        break;
    case RDV:
        res = run_rdv_command(cmd);
        break;
    default:
        fprintf(stderr,"Error -- Unknown command id\n");
        return -1;
    }

    if(res){
        fprintf(stderr,"Error -- Failed to run command ");
        display_command(cmd, stderr);
    }

    return res;
}

/* sends an answer for the command to the client if needed */
/* answer to a command is stored in cmd->answer after the command has
 * been processed. They are different cases
 + The client does not expect any answer (then nothing is sent)
 + The client expect an answer -- 2 cases
  -- The answer is a single msg
  -- The answer is potentially composed of multiple msgs (case of a timeline)
*/
static int answer_command(command_t *cmd)
{    
    /* case of no answer requested by the client */
    if(!cmd->answer_exp){
        if(cmd->answer.aset != NULL){
            free(cmd->answer.aset);
        }
        return 0;
    }
    
    /* no msg to be sent */
    if(cmd->answer.size == -2){
        return 0;
    }

    /* a single msg to be sent */
    if(cmd->answer.size == -1){
        /* strlen()+1 because we want to send '\0' in the message */
        if(write_to_client(cmd->key, strlen(cmd->answer.aset->msg)+1, cmd->answer.aset->msg)){
            fprintf(stderr,"Error -- could not send ack for %d\n", cmd->cid);
            free(cmd->answer.aset);
            return -1;
        }
        free(cmd->answer.aset);
        return 0;
    }
    

    /* a set of msgs to be sent */
    /* number of msgs sent first */
    if(write_to_client(cmd->key, sizeof(int), &cmd->answer.size)){
        fprintf(stderr,"Error -- send set size: %d\n", cmd->cid);
        return -1;
    }

    answer_t *item = cmd->answer.aset, *prev;
    int count=0;

    /* send only the last BABBLE_TIMELINE_MAX */
    int to_skip= (cmd->answer.size > BABBLE_TIMELINE_MAX)? cmd->answer.size - BABBLE_TIMELINE_MAX : 0;

    for(count=0; count < to_skip; count++){
        prev=item;
        item = item->next;
        free(prev);
    }
    
    while(item != NULL ){
        if(write_to_client(cmd->key, strlen(item->msg)+1, item->msg)){
            fprintf(stderr,"Error -- could not send set: %d\n", cmd->cid);
            return -1;
        }
        prev=item;
        item = item->next;
        free(prev);
        count++;
    }

    assert(count == cmd->answer.size);
    return 0;
}

//Communication thread
/*Producer thread*/

void* communicator_thread(void *args)
{
    exec_args* arg = (exec_args*) args;
    int newsockfd = arg->newsockfd;
    command_t *cmd;
    char* client_name = arg->client_name;
    unsigned long client_key = arg->client_key;
    char*  recv_buff=NULL;
    int recv_size=0;
    

        /* looping on client commands */
        while((recv_size=network_recv(newsockfd, (void**) &recv_buff)) > 0)
        {
    
            cmd = new_command(client_key);
            if(parse_command(recv_buff, cmd) == -1){
                fprintf(stderr, "Warning: unable to parse message from client %s\n", client_name);
                notify_parse_error(cmd, recv_buff);
            }
            else
            {
                pthread_mutex_lock(&mutex);
                while(count_thread == BUFFER_SIZE)
                {
                    pthread_cond_wait(&empty,&mutex);
                }
       
                BUFFER[c_index] = *cmd;
                c_index = (c_index + 1 )% BUFFER_SIZE;
                count_thread ++; // Count the number of free space in the buffer
                pthread_cond_signal(&full);
                pthread_mutex_unlock(&mutex);
            }

            //free(recv_buff);
            //free(cmd);
        
             }
        if(client_name[0] != 0)
        {
            cmd = new_command(client_key);
            cmd->cid= UNREGISTER;
            
            if(unregisted_client(cmd)){
                fprintf(stderr,"Warning -- failed to unregister client %s\n",client_name);
            }
            //free(cmd);
        

        
        

    }

   

    
    
    pthread_exit(NULL);


}


// Execution thread 
/*Consumer thread*/
void* exec_thread()
{
    command_t *cmd = malloc(sizeof(command_t));
    
    while(1){
        pthread_mutex_lock(&mutex);
        while (count_thread == 0)
        {
            pthread_cond_wait(&full,&mutex);
        }


        
        command_t* temp= &BUFFER[e_index];
        cmd = (command_t*)memcpy((void *)cmd,(void *)temp,sizeof(command_t));// Create a deep copy
        cmd->answer.aset=NULL;
        fprintf(stderr,  "%s\n", cmd->msg);
        
        e_index = (e_index + 1 ) % BUFFER_SIZE;
        count_thread--;
        pthread_cond_signal(&empty);
        pthread_mutex_unlock(&mutex);


        unsigned long client_key = cmd->key;
        if(process_command(cmd) == -1){
        fprintf(stderr, "Warning: unable to process command from client %lu\n", client_key);
                }
        fprintf(stderr, "A %d\n", cmd->answer_exp);
        if(answer_command(cmd) == -1){
        fprintf(stderr, "Warning: unable to answer command from client %lu\n", client_key);
                }
    }

}



int main(int argc, char *argv[])
{
    int sockfd, newsockfd;
    int portno=BABBLE_PORT;
    char* recv_buff=NULL;
    int recv_size=0;
    char client_name[BABBLE_ID_SIZE+1];
    unsigned long client_key=0;
    command_t *cmd;
    pthread_t tid_c;
    pthread_t tid_e;
    
    //tid_c = (pthread_t*)malloc(sizeof(pthread_t));
    //tid_e = (pthread_t*)malloc(sizeof(pthread_t));
    BUFFER = (command_t*)malloc(BUFFER_SIZE*sizeof(command_t));


    
    int opt;
    int nb_args=1;


    while ((opt = getopt (argc, argv, "+p:")) != -1)
    {
        switch (opt){
        case 'p':
            portno = atoi(optarg);
            nb_args+=2;
            break;
        case 'h':
        case '?':
        default:
            display_help(argv[0]);
            return -1;
        }
    }
    
    if(nb_args != argc)
    {
        display_help(argv[0]);
        return -1;
    }

    server_data_init();

    if((sockfd = server_connection_init(portno)) == -1)
    {
        return -1;
    }

    printf("Babble server bound to port %d\n", portno); 
    if(pthread_create(&tid_e,NULL,exec_thread,NULL) != 0){
        fprintf(stderr,"Failed to create thread for file descriptor ");
    }   
    
    /* main server loop */
    while(1)
    {

        client_connected++;
        exec_args* parameters;
        parameters = (exec_args*)malloc (sizeof(exec_args));

         /* let's store the key locally */
         //client_key = cmd->key;



        if((newsockfd= server_connection_accept(sockfd))==-1)
        {
            return -1;
        }

        bzero(client_name, BABBLE_ID_SIZE+1);
        if((recv_size = network_recv(newsockfd, (void**)&recv_buff)) < 0)
        {
            fprintf(stderr, "Error -- recv from client\n");
            close(newsockfd);
            continue;
        }
        
        cmd = new_command(0);

       

        if(parse_command(recv_buff, cmd) == -1 || cmd->cid != LOGIN)
        {
            fprintf(stderr, "Error -- in LOGIN message\n");
            close(newsockfd);
            free(cmd);
            continue;
        }
       
       
         /* before processing the command, we should register the
         * socket associated with the new client; this is to be done only
         * for the LOGIN command */
        cmd->sock = newsockfd;
       
        if(process_command(cmd) == -1){
            fprintf(stderr, "Error -- in LOGIN\n");
            close(newsockfd);
            free(cmd);
            continue;    
        }
        
      

        /* notify client of registration */
        
        if(answer_command(cmd) == -1){
            fprintf(stderr, "Error -- in LOGIN ack\n");
            close(newsockfd);
            free(cmd);
            continue;
        }
        
        
        
        strncpy(client_name, cmd->msg, BABBLE_ID_SIZE);
        //free(recv_buff);
        //free(cmd);
       
        parameters->newsockfd = newsockfd;
        parameters->client_name = client_name;
        parameters->client_key = cmd->key;
        
        pthread_create(&tid_c,NULL,communicator_thread,(void *)parameters);
        

    }

     
    
    
    close(sockfd);
    return 0;
}

