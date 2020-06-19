/*
Autore:         Manuele Dentello
Descrizione:	Chat server con socket e thread. Messaggi pubblici, privati e canali, categorizzazione degli utenti
Istruzioni:     1. "gcc server.c -pthread -o server"
                2. "./server" 
                3. ""telnet IP 5000"
                Con VS Code attivare sessione WSL remota, avviare build task e aprire terminali
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PORT 5000
#define LISTEN_QUEUE 10
#define MAX_CLIENTS 100	
#define MAX_CHANNELS 10

#define BUFF_OUT_SIZE 1024
#define BUFF_IN_SIZE 924
#define NICK_LENGTH 32
#define CHANNEL_LENGTH 64
#define TOPIC_LENGTH 128
#define MSG_LENGTH 512

#define CLRSCR "\e[1;1H\e[2J"
#define JOIN 1
#define LEAVE 0
#define FRAME "--------------------\n"

// STRUTTURA DEL CLIENT
typedef struct {
    int conn_fd;        // file descriptor della connessione
    struct sockaddr_in ip;      // Indirizzo client
    char nick[NICK_LENGTH];     // Nome client
    int channel_id;
} client_t;

int client_c = 0;	// variabile globale per il conteggio dei client connessi
client_t *clients[MAX_CLIENTS];		// memoria di tutti i client connessi
char channels[MAX_CHANNELS][CHANNEL_LENGTH] = {"home","university","gaming"};        // elenco dei canali
char topics[MAX_CHANNELS][TOPIC_LENGTH] = {"This is channel #home", "This is channel #university", "This is channel #gaming"};
pthread_mutex_t mutex_clients = PTHREAD_MUTEX_INITIALIZER;	//semaforo per accesso alla memoria dei client
pthread_mutex_t mutex_topics = PTHREAD_MUTEX_INITIALIZER;	// semaforo per argomento canale

// GESTIONE CLIENT
void *handle_client(void *arg);
void client_add(client_t *client);
void client_delete(int conn_fd);

// COMUNICAZIONE
void send_msg_server(char *buff_out);
int recv_msg_client(char *buff_out, client_t *client);
void send_msg_pub(char *buff_out, client_t *client);
int send_msg_priv(char *buff_out, client_t *client);
void send_msg_join_leave(client_t *client, int status);
void send_msg_channel(client_t *client, int status);

// COMANDI
void command_channel(client_t *client);
void command_nick(client_t *client);
void command_pm(client_t *client);
void command_topic(client_t *client);
void command_info(client_t *client);
void command_help(client_t *client);
void command_me(client_t *client);

// UTILITY
void print_welcome(client_t *client);
int get_channel_id(char *channel_name);
void print_topic(client_t *client);
client_t *get_client(char *nick);
void crlf_to_string(char *s);
void print_clients();
int is_nick_used(char *nick);
void get_nick(client_t *client);

/* AGGIUNTA DI UN CLIENT IN MEMORIA */
void client_add(client_t *client) {
    int i;

    pthread_mutex_lock(&mutex_clients);
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i]) {      // se il puntatore è vuoto
                clients[i] = client;
                break;
            }
        }
        client_c++;
    pthread_mutex_unlock(&mutex_clients);
}

/* ELIMINAZIONE DEL CLIENT DALLA MEMORIA */
void client_delete(int conn_fd) {
    int i;

    pthread_mutex_lock(&mutex_clients);
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i]) {
                if (clients[i]->conn_fd == conn_fd) {
                    clients[i] = NULL;
                    break;
                }
            }
        }
        client_c--;
    pthread_mutex_unlock(&mutex_clients);
}

/* INVIO DI UN MESSAGGIO PUBBLICO DAL SERVER */
void send_msg_server(char *buff_out) {
    pthread_mutex_lock(&mutex_clients);
    for (int i = 0; i <MAX_CLIENTS; i++){
        if (clients[i]) {
            if (write(clients[i]->conn_fd, buff_out, strlen(buff_out)) < 0) {
                perror("[send_msg_server] Write to recipient descriptor failed");
            }
        }
        else
            break;
    }
    pthread_mutex_unlock(&mutex_clients);
}

/* RICEZIONE DI UN MESSAGGIO DAL CLIENT */
int recv_msg_client(char *buff_in, client_t *client) {
    int resp_len;
    int result;

    resp_len = read(client->conn_fd, buff_in, BUFF_IN_SIZE - 1);     // -1 perché lascio spazio per il terminatore di stringa
    if (resp_len < 0) {
        perror("[read] Read from recipient descriptor failed");
        result = 0;
    }
    else {
        buff_in[resp_len] = '\0';       // inserisco il terminatore stringa per poter elaborare l'input
        crlf_to_string(buff_in);        // se nel buffer trovo il carattere invio, tronco la stringa
        result = 1;
    }
    return result;
}

/* INVIO DI UN MESSAGGIO PRIVATO */
int send_msg_priv(char *buff_out, client_t *client) {
    int result;

    if (write(client->conn_fd, buff_out, strlen(buff_out)) < 0) {
        perror("[send_msg_priv] Write to recipient descriptor failed");
        result = 0;
    }
    else
        result = 1;
    return result;
}

/* INVIO DI UN MESSAGGIO PUBBLICO DAL CLIENT IN CANALE */
void send_msg_pub(char *buff_out, client_t *client){
    pthread_mutex_lock(&mutex_clients);
        for (int i = 0; i <MAX_CLIENTS; i++){
            if (clients[i]) {
                if (clients[i]->conn_fd != client->conn_fd && clients[i]->channel_id == client->channel_id ) {
                    if (write(clients[i]->conn_fd, buff_out, strlen(buff_out)) < 0) {
                        perror("[send_msg_pub] Write to recipient descriptor failed");
                    }
                }
            }
            else
                break;      // se non si trovano client significa che non ce ne sono più, esco
        }
    pthread_mutex_unlock(&mutex_clients);
}

/* INVIO DI UN MESSAGGIO PER ENTRATA/USCITA UTENTE */
void send_msg_join_leave(client_t *client, int status){
    char buff_out[BUFF_OUT_SIZE];

    if (status)
        sprintf(buff_out, "<SERVER> @%s joined the chat\n", client->nick);
    else
        sprintf(buff_out, "<SERVER> @%s left the chat\n", client->nick);
    pthread_mutex_lock(&mutex_clients);
        for (int i = 0; i <MAX_CLIENTS; i++){
            if (clients[i]) {   
                if (clients[i]->conn_fd != client->conn_fd) {
                    if (write(clients[i]->conn_fd, buff_out, strlen(buff_out)) < 0) {
                        perror("[send_msg_join_leave] Write to recipient descriptor failed");
                    }
                }
            }
            else
                break;
        }
    pthread_mutex_unlock(&mutex_clients);
}

/* INVIO DI UN MESSAGGIO PER ENTRATA/USCITA DAL CANALE */
void send_msg_channel(client_t *client, int status) {
    char buff_out[BUFF_OUT_SIZE];

    if (status)
        sprintf(buff_out, "<SERVER> %s joined the channel\n", client->nick);
    else
        sprintf(buff_out, "<SERVER> %s left the channel\n", client->nick);
    pthread_mutex_lock(&mutex_clients);
        for (int i = 0; i <MAX_CLIENTS; i++){
            if (clients[i]) {   
                if (clients[i]->conn_fd != client->conn_fd && clients[i]->channel_id == client->channel_id) {
                    if (write(clients[i]->conn_fd, buff_out, strlen(buff_out)) < 0) {
                        perror("[send_msg_channel] Write to recipient descriptor failed");
                    }
                }
            }
            else
                break;
        }
    pthread_mutex_unlock(&mutex_clients);
}

/* COMANDO DI CAMBIO CANALE */
void command_channel(client_t *client) {
    char *param;
    char channel[CHANNEL_LENGTH];
    char buff_out[BUFF_OUT_SIZE];
    int channel_id;

    send_msg_channel(client, LEAVE);
    param = strtok(NULL, " ");      // se parametro è NULL continua a suddividere la stringa di prima
    if (param) {
        strcpy(channel, param);
        channel_id = get_channel_id(channel); 
        if (channel_id < 0) {
            sprintf(buff_out, "<SERVER> There is no channel with that name\n");
            send_msg_priv(buff_out, client);
        }
        else {
            client->channel_id = channel_id;
            sprintf(buff_out, "<SERVER> Channel changed to #%s\n", channel);
            send_msg_priv(buff_out, client);
            print_topic(client);
        }
    }
    else
        send_msg_priv("<SERVER> Channel name cannot be null\n", client);
    send_msg_channel(client, JOIN);
}

// COMANDO DI CAMBIO NICKNAME
void command_nick(client_t *client) {
    char *param;
    char buff_out[BUFF_OUT_SIZE];
    char old_nick[NICK_LENGTH];

    param = strtok(NULL, " ");
    if (param) {
        if (strlen(param) > NICK_LENGTH) {
            sprintf(buff_out, "<SERVER> The nickname max length is %d characters\n", NICK_LENGTH);
            send_msg_priv(buff_out, client);
        }
        else if (is_nick_used(param)) {
            sprintf(buff_out, "<SERVER> Nickname is already used\n");
            send_msg_priv(buff_out, client);
        }
        else {
            strcpy(old_nick, client->nick);
            strcpy(client->nick, param);
            sprintf(buff_out, "<SERVER> @%s changed nickname to @%s\n", old_nick, client->nick);
            send_msg_server(buff_out);
        }
    } else {
        send_msg_priv("<SERVER> nickname cannot be null\n", client);
    }
}

/* COMANDO MESSAGGIO PRIVATO */
void command_pm(client_t *client) {
    char *param;
    char buff_out[BUFF_OUT_SIZE];
    char nick[NICK_LENGTH];
    client_t *recipient;

    param = strtok(NULL, " ");    // con parametro NULL pesco il prossimo token della stringa precedentemente tokenizzata
    if (param) {        // se non è vuoto
        strcpy(nick, param);
        recipient = get_client(nick);
        if (recipient) {     // se esiste il nick
            param = strtok(NULL, " ");
            if (param) {        // se il messaggio non è vuoto
                sprintf(buff_out, "[PM][@%s]", client->nick);
                while (param != NULL) {
                    strcat(buff_out, " ");
                    strcat(buff_out, param);
                    param = strtok(NULL, " ");
                }
                strcat(buff_out, "\n");
                if (strlen(buff_out) > MSG_LENGTH) {        // se il messaggio va oltre la dimensione prefissata
                    sprintf(buff_out, "<SERVER> The max message length is %d characters\n", MSG_LENGTH);
                    send_msg_priv(buff_out, client);
                }
                else
                    send_msg_priv(buff_out, recipient);     // invio del messaggio
            }
            else
                send_msg_priv("<SERVER> Message cannot be null\n", client);
        }
        else {
            sprintf(buff_out, "<SERVER> No users online with that nickname\n");
            send_msg_priv(buff_out, client);
        }       
    }
    else
        send_msg_priv("<SERVER> Recipient cannot be null\n", client);
}

/* COMANDO TOPIC */
void command_topic(client_t *client) {
    char *param;
    char buff_out[BUFF_OUT_SIZE];
    char topic[TOPIC_LENGTH];

    param = strtok(NULL, " ");
    if (param) {
        strcpy(topic, param);
        param = strtok(NULL, " ");
        while (param != NULL) {
            strcat(topic, " ");
            strcat(topic, param);
            param = strtok(NULL, " ");
        }
        if (strlen(topic) > TOPIC_LENGTH) {
            sprintf(buff_out, "<SERVER> Max topic length is %d characters\n", TOPIC_LENGTH);
            send_msg_priv(buff_out, client);
        }
        else {
            pthread_mutex_lock(&mutex_topics);
                strcpy(topics[client->channel_id], topic);
            pthread_mutex_unlock(&mutex_topics);
            sprintf(buff_out, "<SERVER> Topic changed!\n");
            send_msg_priv(buff_out, client);
            sprintf(buff_out, "<SERVER> @%s changed the topic of the channel to \"%s\"\n", client->nick, topic);
            send_msg_pub(buff_out, client);
        }
    } else {
        pthread_mutex_lock(&mutex_topics);
            strcpy(topics[client->channel_id], "");
        pthread_mutex_unlock(&mutex_topics);
        sprintf(buff_out, "<SERVER> Topic changed!\n");
        send_msg_priv(buff_out, client);
        sprintf(buff_out, "<SERVER> @%s changed the topic of the channel to \"%s\"\n", client->nick, topic);
        send_msg_pub(buff_out, client);
    }
}

/* COMANDO INFO SERVER */
void command_info(client_t *client) {
    char buff_out[BUFF_OUT_SIZE];
    char buff_temp[BUFF_OUT_SIZE];
    int i;
    int j;

    pthread_mutex_lock(&mutex_clients);
    pthread_mutex_lock(&mutex_topics);
        sprintf(buff_out, FRAME);           
        strcat(buff_out, "<SERVER> CHAT SERVER INFO");
        sprintf(buff_temp, "\nUsers online: %d\n", client_c);        // uso altro buffer per convertire l'intero
        strcat(buff_out, buff_temp);
        for (i = 0; i < MAX_CHANNELS; i++) {
            if (strlen(channels[i])) {      // se il canale è stato impostato
                strcat(buff_out, "\nChannel: #");
                strcat(buff_out, channels[i]);
                strcat(buff_out, "\nTopic: \"");
                strcat(buff_out, topics[i]);
                strcat(buff_out, "\"");   
                strcat(buff_out, "\nUsers: ");   
                for (j = 0; j < MAX_CLIENTS; j++) { 
                    if (clients[j] && clients[j]->channel_id == i) {
                        if (j == 0) {
                            strcat(buff_out, "@");
                            strcat(buff_out, clients[j]->nick);
                        }
                        else {
                            strcat(buff_out, ", @");
                            strcat(buff_out, clients[j]->nick);
                        }
                    }
                }
                strcat(buff_out, "\n");
            }
        }
        strcat(buff_out, FRAME);
        send_msg_priv(buff_out, client); 
    pthread_mutex_unlock(&mutex_clients);
    pthread_mutex_unlock(&mutex_topics);
}

/* COMANDO DI AIUTO */
void command_help(client_t *client) {
    char buff_out[BUFF_OUT_SIZE];
    
    sprintf(buff_out, FRAME);
    strcat(buff_out, "<SERVER> COMMANDS\n");
    strcat(buff_out, "<SERVER> /channel <channel>       Join specific channel\n");
    strcat(buff_out, "<SERVER> /help                    View help page\n");
    strcat(buff_out, "<SERVER> /info                    View users, channels and topics\n");
    strcat(buff_out, "<SERVER> /me                      View your status on the server\n");
    strcat(buff_out, "<SERVER> /nick <new_nick>         Change your nickname (no whitespaces)\n");
    strcat(buff_out, "<SERVER> /pm <nick> <message>     Send message to a specific user\n");
    strcat(buff_out, "<SERVER> /quit                    Quit the chat\n");
    strcat(buff_out, "<SERVER> /topic <message>         Set the topic for the joined channel\n");
    strcat(buff_out, FRAME);
    send_msg_priv(buff_out, client);
}

/* COMANDO PER INFO PERSONALI */
void command_me(client_t *client) {
    char buff_out[BUFF_OUT_SIZE];

    pthread_mutex_lock(&mutex_topics);
        sprintf(buff_out, FRAME);
        strcat(buff_out, "<SERVER> PERSONAL INFO\n");
        strcat(buff_out, "Nick: @");
        strcat(buff_out, client->nick);
        strcat(buff_out, "\nIP: ");
        strcat(buff_out, inet_ntoa(client->ip.sin_addr));
        strcat(buff_out, "\nChannel: #");
        strcat(buff_out, channels[client->channel_id]);
        strcat(buff_out, "\nTopic: \"");
        strcat(buff_out, topics[client->channel_id]);
        strcat(buff_out, "\"");
        strcat(buff_out, "\n");
        strcat(buff_out, FRAME);
        send_msg_priv(buff_out, client);
    pthread_mutex_unlock(&mutex_topics);
}

/* STAMPA DELLA SCHERMATA DI BENVENUTO */
void print_welcome(client_t *client){
    char buff_out[BUFF_OUT_SIZE];
    
    send_msg_priv(CLRSCR, client);
    sprintf(buff_out, "<SERVER> @%s, welcome to the chat server! Write \"/help\" for commands\n", client->nick);
    send_msg_priv(buff_out, client);
    print_topic(client);
}

/* AGGUNTA TERMINATORE STRINGA */
void crlf_to_string(char *s){
    while (*s != '\0') {
        if (*s == '\r' || *s == '\n')
            *s = '\0';
        s++;
    }
}

/* RICERCA ID A PARTIRE DAL NOME */
client_t *get_client(char *nick) {
    int i;
    client_t *client;

    client = NULL;
    pthread_mutex_lock(&mutex_clients);
    for (i = 0; i < MAX_CLIENTS; i++){
        if (clients[i]) {
            if (!strcmp(clients[i]->nick, nick))
                client = clients[i];
        }
    }
    pthread_mutex_unlock(&mutex_clients);
    return client;
}

/* RICERCA ID CANALE */
int get_channel_id(char *channel_name) {
    int i;
    int channel_id;

    channel_id = -1;
    for (i = 0; i <MAX_CHANNELS; i++){
        if (channels[i]) {
            if (!strcmp(channels[i], channel_name)) {
                channel_id = i;
                break;
            }
        }
    }
    return channel_id;
}

/* STAMPA DEL TOPIC */
void print_topic(client_t *client) {
    char buff_out[BUFF_OUT_SIZE];

    pthread_mutex_lock(&mutex_topics);
        sprintf(buff_out, "<SERVER> The topic is: \"%s\"\n", topics[client->channel_id]);
        send_msg_priv(buff_out, client);
    pthread_mutex_unlock(&mutex_topics);
}

/* STAMPA MEMORIA CLIENT (USATA PER DEBUG) */
void print_clients() {
    int i;
    pthread_mutex_lock(&mutex_clients);
       for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i])
                printf("Client %d\nconn_fd: %d\nnick: %s\nchannel_id: %d\n\n", i, clients[i]->conn_fd, clients[i]->nick, clients[i]->channel_id);
        }
    pthread_mutex_unlock(&mutex_clients);
}

/* RICERCA NICK GIA' UTILIZZATO */
int is_nick_used(char *nick) {
    int i;
    int used = 0;

    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i]) {
            if (!strcmp(clients[i]->nick, nick)) {
                used = 1;
                break;
            }
        }
    }
    return used;
}

/* OTTENIMENTO DI NICK PER NUOVO UTENTE */
void get_nick(client_t *client) {
    char buff_out[BUFF_OUT_SIZE];
    char buff_in[BUFF_IN_SIZE];
    int resp_len;

    while (1) {
        if (!send_msg_priv("<SERVER> Insert your nickname\n", client))
            continue;
        //buff_in[0] = '\0';
        if (!recv_msg_client(buff_in, client))
            continue;

        if (!strcmp(buff_in, "")) {
            send_msg_priv("Nickname cannot be null!\n", client);
            continue;
        }
        else if (resp_len > NICK_LENGTH) {
            sprintf(buff_out, "The nickname max length is %d\n", NICK_LENGTH);
            send_msg_priv(buff_out, client);
            continue;
        }
        else if (is_nick_used(buff_in)) {
            send_msg_priv("<SERVER> Nickname is already used\n", client);       
            continue;
        }
        else
            break;   
    }
    strcpy(client->nick, buff_in);
}

/* THREAD DEL CLIENT */
void *handle_client(void *arg){
    char buff_out[BUFF_OUT_SIZE];
    char buff_in[BUFF_IN_SIZE];
    char *command;
    char *param; 
    char nick[NICK_LENGTH];
    char old_nick[NICK_LENGTH];
    char channel[CHANNEL_LENGTH];
    char topic[TOPIC_LENGTH];
    int resp_len;
    client_t *client = (client_t *)arg;

    // chiedo il nick
    get_nick(client);

    printf("@%s [IP: %s] joined the server\n", client->nick, inet_ntoa(client->ip.sin_addr));
    send_msg_join_leave(client, JOIN);

    //Messaggio di welcome
    print_welcome(client);

    // Ricezione input dal client
    while (1) {
        if (!recv_msg_client(buff_in, client))
            continue;

        if (!strlen(buff_in)) {
            continue;	// ignoro buffer vuoto
        }

        if (buff_in[0] != '/') { // mando messaggio pubblico
            printf("@%s is sending a public message in channel #%s\n", client->nick, channels[client->channel_id]);
            if (strlen(buff_out) > MSG_LENGTH) {      // se il messaggio va oltre la dimensione prefissata
                sprintf(buff_out, "<SERVER> The max message length is %d characters\n", MSG_LENGTH);
                send_msg_priv(buff_out, client);    
            }
            else {
        	    sprintf(buff_out, "[@%s] %s\n", client->nick, buff_in);
            	send_msg_pub(buff_out, client);
            }
        }
        else {
    		command = strtok(buff_in," ");	// srttoken suddivide stringa in token in base a delimitatore
        	if (!strcmp(command, "/quit")) {	// se il risultato non è 1
                break;
            }
            else if (!strcmp(command, "/channel")) {
                printf("@%s is changing channel\n", client->nick);
                command_channel(client);
            }
            else if (!strcmp(command, "/nick")) {
                printf("@%s is changing nickname\n", client->nick);
                command_nick(client);
            }
            else if (!strcmp(command, "/pm")) {
                printf("@%s is sending a PM\n", client->nick);
                command_pm(client);
            }
            else if (!strcmp(command, "/topic")) {
                printf("@%s is getting the topic of the channel #%s\n", client->nick, channels[client->channel_id]);
                command_topic(client);
            }
            else if(!strcmp(command, "/info")) {
                printf("@%s is getting server info\n", client->nick);
                command_info(client);
            } else if (!strcmp(command, "/help")) {
                printf("@%s is getting help\n", client->nick);
                command_help(client);
            } else if (!strcmp(command, "/me")) {
                printf("@%s is getting personal info\n", client->nick);
                command_me(client);
            } else
                send_msg_priv("<SERVER> Unknown command\n", client);
        }
    }
    printf("@%s [IP: %s] left the server\n", client->nick, inet_ntoa(client->ip.sin_addr));
    send_msg_join_leave(client, LEAVE);
    
    // Chiusura connessione, pulizia memoria e cancellazione thread
    close(client->conn_fd);
    client_delete(client->conn_fd);
    free(client);
    pthread_detach(pthread_self());
}

int main(int argc, char *argv[]) {
	int server_fd;
    int conn_fd;
    int resp_len;
	struct sockaddr_in server_ip;
	struct sockaddr_in client_ip;
	int client_ip_l = sizeof(client_ip); 	// lunghezza indirizzo client (serve per la accept)
    pthread_t t_client;
    pthread_t t_server;
    char buff_out[BUFF_OUT_SIZE];
    char buff_in[BUFF_IN_SIZE];
    long int client_i;

	// Settaggio socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    server_ip.sin_family = AF_INET;
    server_ip.sin_addr.s_addr = htonl(INADDR_ANY);	// INADDR_ANY mi permette di legare il socket a tutte le interfacce del sistema, non solo localhost
    server_ip.sin_port = htons(PORT);
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
        perror("Socket options failed\n");
        exit(EXIT_FAILURE);
    }
    if (bind(server_fd, (struct sockaddr*)&server_ip, sizeof(server_ip)) < 0) {
        perror("Socket binding failed");
        return EXIT_FAILURE;	//stdlib
    }
    if (listen(server_fd, LISTEN_QUEUE) < 0) {
        perror("Socket listening failed");
        return EXIT_FAILURE;
    }
    printf("<[SERVER STARTED]>\n");

    // accettazione dei client
     while (1) {
        conn_fd = accept(server_fd, (struct sockaddr*)&client_ip, &client_ip_l);

        // Controllo su massimo numero di client
         if ((client_c + 1) == MAX_CLIENTS) {
            printf("<Max clients reached, IP %s rejected>\n", inet_ntoa(client_ip.sin_addr));
            close(conn_fd);
            continue;
        }

        // Settaggio del client
        client_t *client = (client_t *)malloc(sizeof(client_t));
        client->ip = client_ip;	// utilizzo l'operatore client-> al posto di (*client).
        client->conn_fd = conn_fd;
        client->channel_id = 0;
        strcpy(client->nick, "");   

        // Aggiungo client alla coda e creo nuovo thread
        client_add(client);
        pthread_create(&t_client, NULL, &handle_client, (void*)client);   // la funzione richiede il passaggio parametri tramite void
    }
    return EXIT_SUCCESS;
}