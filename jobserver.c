#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/wait.h>

#include "socket.h"
#include "jobprotocol.h"


#define QUEUE_LENGTH 5
#define MAX_CLIENTS 20

#ifndef JOBS_DIR
    #define JOBS_DIR "jobs/"
#endif

int get_highest_fd(int listen_fd, Client *clients, JobList *job_list);
int setup_new_client(int listen_fd, Client *clients);
int process_client_request(Client *client, JobList *job_list, fd_set *all_fds);
int process_job_output(JobNode *job_node, int fd, Buffer *buffer, char* format);
int process_jobs(JobList *job_list, fd_set *current_fds, fd_set *all_fds);
int read_to_buf2(int fd, Buffer* buf);
int printjob(JobList *jobs);
void sigint_handler(int code);
void clean_exit(int listen_fd, Client *clients, JobList *job_list, int exit_status);

int sigint_received;
JobList jobs;
Client clients[MAX_CLIENTS];
int listennfd;

int main(void) {
    // This line causes stdout and stderr not to be buffered.
    // Don't change this! Necessary for autotesting.
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    struct sockaddr_in *self = init_server_addr(PORT);
    int listenfd = setup_server_socket(self, QUEUE_LENGTH);
    listennfd = listenfd;
    for (int index = 0; index < MAX_CLIENTS; index++) {
        clients[index].socket_fd = -1;
    }

    jobs.first=NULL;
    jobs.count=0;

    //signal(SIGINT, sigint_handler);
    struct sigaction newact;
    sigemptyset (&newact.sa_mask);
    newact.sa_flags = 0;
    newact.sa_handler = sigint_handler;
    sigaction(SIGINT, &newact, NULL);
    //JobList job_list;
    //int fd;

    // JobNode *first = malloc(sizeof(JobNode));
    // JobNode *second = malloc(sizeof(JobNode));
    // JobNode *third = malloc(sizeof(JobNode));
    //
    // first->pid = 1;
    // first->next = NULL;
    // second->pid = 2;
    // second->next = NULL;
    // third->pid = 3;
    // third->next = NULL;
    //
    // add_job(&jobs, first);
    // add_job(&jobs, second);
    // add_job(&jobs, third);
    //
    // printjob(&jobs);
    // printf("removal");
    // second->dead = 1;
    // remove_job(&jobs, 1);
    //
    // printjob(&jobs);
    //
    // return 0;

    int max_fd = listenfd;
    fd_set all_fds, read_fds;
    FD_ZERO(&all_fds);
    FD_SET(listenfd, &all_fds);

    //sigset_t set ,oldset;
    //sigemptyset(&set);
    //sigaddset(&set, SIGINT);
     /* ... Critical operation ... */


    while(1){
      //numfd = get_highest_fd(listenfd, clients, NULL) + 1;
      read_fds = all_fds;
      //sigprocmask (SIG_BLOCK, &set, &oldset);
      if (select(max_fd+1, &read_fds, NULL, NULL, NULL) == -1) {
                perror("select");
                exit(1);
      }
      if (FD_ISSET(listenfd, &read_fds)){
        int client_fd = setup_new_client(listenfd, clients);
        if (client_fd < 0) {
            continue;
        }
        if (client_fd > max_fd) {
            max_fd = client_fd;
        }
        FD_SET(client_fd, &all_fds);
        printf("connection received %d \n", client_fd);
      }

      for (int index = 0; index < MAX_CLIENTS; index++) {
          if (clients[index].socket_fd > -1 && FD_ISSET(clients[index].socket_fd, &read_fds)) {
              // Note: never reduces max_fd
              int client_closed = process_client_request(&clients[index], &jobs, &all_fds);
              if (client_closed < 0) {
                  FD_CLR(clients[index].socket_fd, &all_fds);
                  close(client_closed);
                  printf("Client %d disconnected\n", client_closed);
              } else {
                if(client_closed > max_fd){
                  max_fd = client_closed;
                }
                continue;
                  //printf("Echoing message from client %d\n", clients[index].socket_fd);
                  //printf("[CLIENT %d] %s\n", clients[index].socket_fd, (clients[index].buffer).buf);
              }
          }
      }
      //printf("going again\n");
      // JobNode *joby = jobs.first;
      // for(int i = 0; i < MAX_JOBS && joby!=NULL; i++){
      //   if(joby->dead != 1){
      //     printf("job %d here", joby->stdout_fd);
      //   }
      //   joby=joby->next;
      // }
      process_jobs(&jobs, &read_fds, &all_fds);
      //sigprocmask (SIG_SETMASK, &oldset, NULL);
    }
    /* TODO: Initialize job and client tracking structures, start
     * accepting connections. Listen for messages from both clients
     * and jobs. Execute client commands if properly formatted.
     * Forward messages from jobs to appropriate clients.
     * Tear down cleanly.
     */


    free(self);
    close(listenfd);
    return 0;
}

void sigint_handler(int code) {
  clean_exit(listennfd, clients, &jobs, 1);
}

int remove_job(JobList *jobs, int fd){
      JobNode* temp = jobs->first, *prev;
      if (temp != NULL && temp->dead == 1){
          int status;
          waitpid(temp->pid,&status, WUNTRACED | WCONTINUED);
          char jexit[50]={'\0'};
          if(WIFEXITED(status)){
            temp->wait_status = WEXITSTATUS(status);
            snprintf(jexit, 50, "[JOB %d] Exited with status %d.\r\n", temp->pid, temp->wait_status);
            write(temp->client_fd, jexit, strlen(jexit));
            printf("[JOB %d] Exited with status %d.\n", temp->pid, temp->wait_status);
          }else if(WIFSTOPPED(status) || WIFSIGNALED(status)){
            snprintf(jexit, 50, "[Job %d] Exited due to signal.\r\n", temp->pid);
            write(temp->client_fd, jexit, strlen(jexit));
            printf("[Job %d] Exited due to signal.\n", temp->pid);
          }
          close(temp->stdout_fd);
          close(temp->stderr_fd);
          jobs->first = temp->next;
          free(temp);
          return 0;
      }
      while (temp != NULL && temp->dead != 1) {
          prev = temp;
          temp = temp->next;
      }
      if (temp == NULL) return 1;
      int status;
      waitpid(temp->pid,&status, WUNTRACED | WCONTINUED);
      char jexit[50]={'\0'};
      if(WIFEXITED(status)){
        temp->wait_status = WEXITSTATUS(status);
        snprintf(jexit, 50, "[JOB %d] Exited with status %d.\r\n", temp->pid, temp->wait_status);
        write(temp->client_fd, jexit, strlen(jexit));
        printf("[JOB %d] Exited with status %d.\n", temp->pid, temp->wait_status);
      }else if(WIFSTOPPED(status) || WIFSIGNALED(status)){
          snprintf(jexit, 50, "[Job %d] Exited due to signal.\r\n", temp->pid);
          write(temp->client_fd, jexit, strlen(jexit));
          printf("[Job %d] Exited due to signal.\n", temp->pid);
      }
      close(temp->stdout_fd);
      close(temp->stderr_fd);
      prev->next = temp->next;
      free(temp);
      return 0;
  }

  int printjob(JobList *jobs){
    if(jobs->first==NULL){
      printf("empty\n");
      return 0;
    }
    JobNode *job = jobs->first;
    for(int i = 0;job!=NULL && i < 5;i++){
      printf("pid: %d\n", job->pid);
      job = job->next;
    }
    return 0;
  }

// int process_job_output(JobNode *job_node, int fd, Buffer *buffer, char* format){
//   int nread = read_to_buf(fd, buffer);
//   if(nread == 0){
//     return -1;
//   }
//   printf(format, job_node->pid, buffer->buf);
//   char sendbuf[BUFSIZE+1];
//   sendbuf[BUFSIZE]='\0';
//   snprintf(sendbuf, BUFSIZE, format, job_node->pid, buffer->buf);
//   write(job_node->client_fd, sendbuf, strlen(sendbuf));
//   memmove(buffer->buf, buffer->buf+buffer->consumed, BUFSIZE - buffer->consumed);
//   int findd = find_unix_newline(buffer->buf, buffer->inbuf);
//   if(findd!=-1){
//     buffer->buf[findd-2] = '\0';
//   }
//   return 0;
// }

int read_to_buf2(int fd, Buffer* buf){
  //int inbuf = 0;           // How many bytes currently in buffer?
  int room = BUFSIZE - (buf->inbuf);  // How many bytes remaining in buffer?
  char *after = buf->buf + (buf->inbuf);       // Pointer to position after the data in buf

  int nbytes;
  while ((nbytes = read(fd, after, room)) > 0) {
      (buf->inbuf) += nbytes;
      //int where;
      while (find_unix_newline(buf->buf, buf->inbuf) > 0) {
          //buf[where-2] = '\0';
          //printf("Next message: %s\n", buf);
          return 0;
          //inbuf -= where;

          //memmove(buf, buf+where, BUFSIZE - where);


      }
      // Step 5: update after and room, in preparation for the next read.
      room = BUFSIZE - (buf->inbuf);
      after = buf->buf + (buf->inbuf);
  }
  if(nbytes == 0){
    return -1;
  }
  return -2;
}

char* get_next_msg(Buffer *buf, int *msg_len, NewlineType n){
  int where = find_unix_newline(buf->buf, buf->inbuf);
  if(where == -1){
    return NULL;
  }
  //char final[BUFSIZE] = {'\0'};
  //char *final = malloc(sizeof(char)*BUFSIZE);
  //strncpy(final, buf->buf, where);
  //memmove(buf, buf+where, BUFSIZE - where);
  //(buf->inbuf) -= where;
  //final[where - 2] = '\0';
  *msg_len = where;
  //return final;
  (buf->buf)[where-1] = '\0';
  *msg_len=where;
  return buf->buf;
}

int process_job_output(JobNode *job_node, int fd, Buffer *buffer, char* format){
  //int nread =
  int msg_len;
  int readd = read_to_buf2(fd, buffer);
  if(buffer->inbuf > BUFSIZE-11){
    char bfull[BUFSIZE] = {'\0'};
    snprintf(bfull, BUFSIZE, "*(SERVER)* Buffer from job %d is full. Aborting job.\r\n", job_node->pid);
    write(job_node->client_fd, bfull, strlen(bfull));
    printf("*(SERVER)* Buffer from job %d is full. Aborting job.\n", job_node->pid);
    return fd;
  }
  char* final = get_next_msg(buffer, &msg_len, NEWLINE_LF);
  if(readd == -1 && final == NULL){
    return fd;
  }
  char rn[] = "\r\n";
  char nn[] = "\n";
  printf(format, job_node->pid, final, nn);
  char sendbuf[BUFSIZE+1];
  sendbuf[BUFSIZE]='\0';
  snprintf(sendbuf, BUFSIZE, format, job_node->pid, final, rn);
  write(job_node->client_fd, sendbuf, strlen(sendbuf));
  //free(final);
  (buffer->inbuf) -= msg_len;
  memmove(buffer->buf, (buffer->buf)+msg_len, BUFSIZE - msg_len);
  return 0;

}

int process_jobs(JobList *job_list, fd_set *current_fds, fd_set *all_fds){
  JobNode *fjob = job_list->first;
  if(fjob==NULL){
    return 0;
  }
  for(; fjob!=NULL; fjob = fjob->next) {
      if (fjob->dead != 1 && FD_ISSET(fjob->stdout_fd, current_fds)) {
        //printf("selected %d\n", fjob->stdout_fd);
          // Note: never reduces max_fd
          int client_closed = process_job_output(fjob, fjob->stdout_fd, &(fjob->stdout_buffer), "[JOB %d] %s%s");
          if (client_closed > 0) {
              FD_CLR(fjob->stdout_fd, all_fds);
              FD_CLR(fjob->stderr_fd, all_fds);

              //remove_job(job_list, client_closed);
              fjob->dead = 1;
              // int status;
              // waitpid(fjob->pid,&status, WNOHANG);
              // char jexit[50]={'\0'};
              // if(WIFEXITED(status)){
              //   fjob->wait_status = WEXITSTATUS(status);
              //   snprintf(jexit, 50, "[JOB %d] Exited with status %d.\r\n", fjob->pid, fjob->wait_status);
              //   write(fjob->client_fd, jexit, strlen(jexit));
              //   }
              // close(client_closed);
              remove_job(job_list, client_closed);
              //job_list->count--;
              //printf("job done stdout %d\n", client_closed);
              break;
          } else {
            //fjob = fjob->next;
              //printf("Echoing message from client %d\n", clients[index].socket_fd);
              //printf("[CLIENT %d] %s\n", clients[index].socket_fd, (clients[index].buffer).buf);
          }
      }
      if (fjob->dead != 1 && FD_ISSET(fjob->stderr_fd, current_fds)) {
        //printf("selected %d\n", fjob->stderr_fd);
          // Note: never reduces max_fd
          int client_closed = process_job_output(fjob, fjob->stderr_fd, &(fjob->stderr_buffer), "*(JOB %d)* %s%s");
          if (client_closed < 0) {
              FD_CLR(fjob->stdout_fd, all_fds);
              FD_CLR(fjob->stderr_fd, all_fds);
              close(client_closed);
              //remove_job(job_list, client_closed);
              fjob->dead = 1;
              // int status;
              // waitpid(fjob->pid,&status, WNOHANG);
              // char jexit[50]={'\0'};
              // if(WIFEXITED(status)){
              //   fjob->wait_status = WEXITSTATUS(status);
              //   snprintf(jexit, 50, "[JOB %d] Exited with status %d.\r\n", fjob->pid, fjob->wait_status);
              //   write(fjob->client_fd, jexit, strlen(jexit));
              //   }
              remove_job(job_list, client_closed);

              //job_list->count--;
              //printf("job done stdout %d\n", client_closed);
              break;
          } else {
            //fjob = fjob->next;
              //printf("Echoing message from client %d\n", clients[index].socket_fd);
              //printf("[CLIENT %d] %s\n", clients[index].socket_fd, (clients[index].buffer).buf);
          }
      }
      //fjob = fjob->next;
  }
  return 1;
}

int add_job(JobList *jobs, JobNode *job ){
  if(jobs->first == NULL){
    jobs->first = job;
    return 0;
  }
  JobNode *jfirst = jobs->first;
  for(int i = 0;i < MAX_JOBS;i++){
    if(jfirst->next == NULL){
      jfirst->next = job;
      return 0;
    }
    jfirst = jfirst->next;
  }
  return -1;
}

JobCommand get_job_command(char* buf){
  char temp[5];
  strncpy(temp, buf, 5);
  if(strstr(temp, "run ")!=NULL){
    //printf("RUN command read: %s\n", buf+4);
    return CMD_RUNJOB;
  }else if(strstr(temp, "kill ")!=NULL){
    //printf("KILL command read: %s\n", buf+5);
    return CMD_KILLJOB;
  }
  else if(strlen(temp) == 4 && strstr(temp, "jobs")!=NULL){
    //printf("JOBS command read:\n");
    return CMD_LISTJOBS;
  }
  return CMD_INVALID;
}

JobNode* start_job(char *exe, char * const flags[]){
  //printf("exe: %s\n", exe);
  //for(int i = 1; flags[i]!= NULL; i++){
    //printf("%s \n", flags[i]);
  //}
  int pout[2];
  int perr[2];
  if(pipe(pout)==-1){
      perror("pipe");
      return NULL;
    }
  if(pipe(perr)==-1){
      perror("pipe");
      return NULL;
    }
  pid_t jobpid = fork();
  if(jobpid < 0){
    perror("fork");
    return NULL;
  }else if(jobpid > 0){
    close(pout[1]);
    close(perr[1]);
    JobNode *job = malloc(sizeof(JobNode));
    job->pid = jobpid;
    job->stdout_fd = pout[0];
    job->stderr_fd = perr[0];
    job->stdout_buffer.inbuf = 0;
    job->stderr_buffer.inbuf = 0;
    job->stdout_buffer.consumed = 0;
    job->stderr_buffer.consumed = 0;
    job->next = NULL;
    return job;
  }else{
    close(pout[0]);
    close(perr[0]);
    if(dup2(pout[1], STDOUT_FILENO) < 0){
      perror("dup2");
      exit(1);
    }
    if(dup2(perr[1], STDERR_FILENO) < 0){
      perror("dup2");
      exit(1);
    }
    char exe_file[BUFSIZE];
    snprintf(exe_file, BUFSIZE, "%s%s", JOBS_DIR, exe);
    execv(exe_file, flags);
    perror("execv");
    //char ss[] = "\r\n";
    //write(perr[1], ss, 2);
    close(pout[1]);
    close(perr[1]);
    exit(1);
  }
  return NULL;
}

int process_client_request(Client *client, JobList *job_list, fd_set *all_fds){
  int jread = read_to_buf(client->socket_fd, &(client->buffer));
  if(jread==0){
    return -1;
  }
  printf("[CLIENT %d] %s\n", client->socket_fd, (client->buffer).buf);
  JobCommand jobtype = get_job_command((client->buffer).buf);
  if(jobtype==CMD_RUNJOB){
    int nargs = 0;
    for(int i = 0; (client->buffer).buf[i] != '\0'; i++){
      if((client->buffer).buf[i]==' '){
        nargs++;
      }
    }
    char *args[nargs+1];
    //char **args = malloc(sizeof(char*)*nargs);
    //for(int i = 0; i < nargs; i++){
      //args[i] = malloc(sizeof(char)*10);
    //}
    char* token = strtok((client->buffer).buf+4, " ");
    int i;
    for(i = 0; token != NULL; i++) {
        args[i] = token;
        token = strtok(NULL, " ");
    }
    args[i] = NULL;
    JobNode *job = start_job(args[0], args);
    add_job(job_list, job);
    job->client_fd = client->socket_fd;
    char jc[30]={'\0'};
    snprintf(jc, 30, "[SERVER] Job %d created\r\n", job->pid);
    write(job->client_fd, jc, strlen(jc));
    FD_SET(job->stdout_fd, all_fds);
    FD_SET(job->stderr_fd, all_fds);
    job_list->count++;
    if(job->stdout_fd > job->stderr_fd){
      return job->stdout_fd;
    }else{
      return job->stderr_fd;
    }
  }else if(jobtype == CMD_LISTJOBS){
    char jobsoutput[BUFSIZE]={'\0'};
    if(job_list->first==NULL){
      strncpy(jobsoutput, "[SERVER] No currently running jobs\r\n", 36);
      printf("%s\n",jobsoutput);
      write(client->socket_fd, jobsoutput, strlen(jobsoutput));
      return 0;
    }
    strncpy(jobsoutput, "[SERVER]", 8);
    printf("[SERVER]");
    JobNode *fjob = job_list->first;
    for (; fjob!=NULL; fjob = fjob->next) {
      write(client->socket_fd, jobsoutput, strlen(jobsoutput));
      snprintf(jobsoutput, BUFSIZE, "%s %d", jobsoutput, fjob->pid);
      printf(" %d", fjob->pid);
    }
    strncat(jobsoutput, "\r\n", 2);
    printf("\n");
    write(client->socket_fd, jobsoutput, strlen(jobsoutput));
    return 0;
  }else if(jobtype == CMD_KILLJOB){
    char koutput[40]={'\0'};
    if(job_list->first==NULL){
      snprintf(koutput, 40, "[SERVER] Job %s not found\r\n", (client->buffer).buf+5);
      printf("%s", koutput);
      write(client->socket_fd, koutput, strlen(koutput));
    }else{
      JobNode *sjob = job_list->first;
      int pidd;
      for (; sjob!=NULL; sjob = sjob->next) {
        pidd = strtol((client->buffer).buf+5, NULL, 10);
        if(sjob->pid == pidd){
          kill(pidd, SIGKILL);
          sjob->dead=1;
          FD_CLR(sjob->stdout_fd, all_fds);
          FD_CLR(sjob->stderr_fd, all_fds);
          remove_job(job_list, 1);
          return 0;
        }
      }
    }
    return 0;
  }
  else if(jobtype == CMD_INVALID){
    char koutput[50]={'\0'};
    snprintf(koutput, 50, "[SERVER] Invalid command: %s\r\n", (client->buffer).buf);
    printf("%s", koutput);
    write(client->socket_fd, koutput, strlen(koutput));
  }
  return jread;
}

int read_to_buf(int fd, Buffer* buf){
  buf->inbuf = 0;           // How many bytes currently in buffer?
  int room = BUFSIZE;  // How many bytes remaining in buffer?
  char *after = buf->buf;

  int nbytes;
  while ((nbytes = read(fd, after, room)) > 0) {
      buf->inbuf += nbytes;
      int where;
      while ((where = find_network_newline(buf->buf, buf->inbuf)) > 0) {
          buf->buf[where-2] = '\0';
          //printf("Next message: %s\n", buf->buf);
          return buf->inbuf;
          buf->inbuf -= where;
          memmove(buf->buf, buf->buf+where, BUFSIZE - where);
      }
      room = BUFSIZE - buf->inbuf;
      after = buf->buf + buf->inbuf;
  }
  if(nbytes == 0){
    return 0;
  }
  return -1;

}

int find_unix_newline(const char *buf, int inbuf){
  for(int i = 0; i < inbuf; i++){
    if(buf[i] == '\n'){
      return i+1;
    }
  }
    return -1;
}

int setup_new_client(int listen_fd, Client *clients){
  int user_index = 0;
  while (user_index < MAX_CLIENTS && clients[user_index].socket_fd != -1) {
      user_index++;
  }

  int client_fd = accept_connection(listen_fd);
  if (client_fd < 0) {
      return -1;
  }

  if (user_index >= MAX_CLIENTS) {
      fprintf(stderr, "server: max concurrent connections\n");
      close(client_fd);
      return -1;
  }
  // char* username = malloc(sizeof(char)*BUF_SIZE);
  // int rread = read(client_fd, username, BUF_SIZE);
  // username[rread-1] = '\0';
  clients[user_index].socket_fd = client_fd;
  //strncpy(usernames[user_index].username, username, strlen(username));
  //usernames[user_index].username = username;
  return client_fd;
}

int get_highest_fd(int listen_fd, Client *clients, JobList *job_list){
  int c = 0;
  int j = 0;
  int final;
  JobNode *current;
  if(clients!=NULL){
    for(int i = 0;i < 5;i++){
      if(clients[i].socket_fd > c){
        c = clients[i].socket_fd;
      }
    }
  }

  if(job_list!=NULL){
    current = job_list->first;
    for(;;){
      if(current->stdout_fd > j){
        current->stdout_fd = j;
      }
      if(current->stderr_fd > j){
        current->stderr_fd = j;
      }
      current = current->next;
    }
  }

  if(c > j && c > listen_fd){
    final = c;
  }else if(j > listen_fd){
    final = j;
  }else{
    final = listen_fd;
  }

  return final;
}

int find_network_newline(const char *buf, int inbuf) {
  for(int i = 0; i < inbuf; i++){
    if(buf[i] == '\r' && buf[i+1]== '\n'){
      return i+2;
    }
  }
    return -1;
}

void clean_exit(int listen_fd, Client *clients, JobList *job_list, int exit_status){
  close(listen_fd);
  char koutput[] = "[SERVER] Shutting down\r\n";

  for (int index = 0; index < MAX_CLIENTS; index++) {
      if(clients[index].socket_fd != -1){
        write(clients[index].socket_fd, koutput, strlen(koutput));
        close(clients[index].socket_fd);
      }
  }

  if(job_list->first==NULL){

  }else{
  JobNode *job = job_list->first;
  for(int i = 0; job!=NULL; i++){
    if(job->dead != 1){
      job->dead = 1;
      job = job->next;
      remove_job(job_list, 1);
    }else{
    job = job->next;
  }
  }
}

printf("[SERVER] Shutting down\n");
exit(exit_status);
}
