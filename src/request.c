#include "io_helper.h"
#include "request.h"
#include "buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define MAXBUF (8192)

int num_threads = DEFAULT_THREADS;
int buffer_max_size = DEFAULT_BUFFER_SIZE;
int scheduling_algo = DEFAULT_SCHED_ALGO;

// Create buffer array and make global
bufferRequest reqarr[MAXBUF] = {0, NULL, 0};
pthread_mutex_t buffer_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t buffer_not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t buffer_not_full = PTHREAD_COND_INITIALIZER;
int buf_size = 0;

//
// Sends out HTTP response in case of errors
//
void request_error(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
  char buf[MAXBUF], body[MAXBUF];

  // Create the body of error message first (have to know its length for header)
  sprintf(body, ""
                "<!doctype html>\r\n"
                "<head>\r\n"
                "  <title>CYB-3053 WebServer Error</title>\r\n"
                "</head>\r\n"
                "<body>\r\n"
                "  <h2>%s: %s</h2>\r\n"
                "  <p>%s: %s</p>\r\n"
                "</body>\r\n"
                "</html>\r\n",
          errnum, shortmsg, longmsg, cause);

  // Write out the header information for this response
  sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
  write_or_die(fd, buf, strlen(buf));

  sprintf(buf, "Content-Type: text/html\r\n");
  write_or_die(fd, buf, strlen(buf));

  sprintf(buf, "Content-Length: %lu\r\n\r\n", strlen(body));
  write_or_die(fd, buf, strlen(buf));

  // Write out the body last
  write_or_die(fd, body, strlen(body));

  // close the socket connection
  close_or_die(fd);
}

//
// Reads and discards everything up to an empty text line
//
void request_read_headers(int fd)
{
  char buf[MAXBUF];

  readline_or_die(fd, buf, MAXBUF);
  while (strcmp(buf, "\r\n"))
  {
    readline_or_die(fd, buf, MAXBUF);
  }
  return;
}

//
// Return 1 if static, 0 if dynamic content (executable file)
// Calculates filename (and cgiargs, for dynamic) from uri
//
int request_parse_uri(char *uri, char *filename, char *cgiargs)
{
  char *ptr;

  if (!strstr(uri, "cgi"))
  {
    // static
    strcpy(cgiargs, "");
    sprintf(filename, ".%s", uri);
    if (uri[strlen(uri) - 1] == '/')
    {
      strcat(filename, "index.html");
    }
    return 1;
  }
  else
  {
    // dynamic
    ptr = index(uri, '?');
    if (ptr)
    {
      strcpy(cgiargs, ptr + 1);
      *ptr = '\0';
    }
    else
    {
      strcpy(cgiargs, "");
    }
    sprintf(filename, ".%s", uri);
    return 0;
  }
}

//
// Fills in the filetype given the filename
//
void request_get_filetype(char *filename, char *filetype)
{
  if (strstr(filename, ".html"))
    strcpy(filetype, "text/html");
  else if (strstr(filename, ".gif"))
    strcpy(filetype, "image/gif");
  else if (strstr(filename, ".jpg"))
    strcpy(filetype, "image/jpeg");
  else
    strcpy(filetype, "text/plain");
}

//
// Handles requests for static content
//
void request_serve_static(int fd, char *filename, int filesize)
{
  int srcfd;
  char *srcp, filetype[MAXBUF], buf[MAXBUF];

  request_get_filetype(filename, filetype);
  srcfd = open_or_die(filename, O_RDONLY, 0);

  // Rather than call read() to read the file into memory,
  // which would require that we allocate a buffer, we memory-map the file
  srcp = mmap_or_die(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
  close_or_die(srcfd);

  // put together response
  sprintf(buf, ""
               "HTTP/1.0 200 OK\r\n"
               "Server: OSTEP WebServer\r\n"
               "Content-Length: %d\r\n"
               "Content-Type: %s\r\n\r\n",
          filesize, filetype);

  write_or_die(fd, buf, strlen(buf));

  //  Writes out to the client socket the memory-mapped file
  write_or_die(fd, srcp, filesize);
  munmap_or_die(srcp, filesize);
}

/**
 * Fetches the requests from the buffer and handles them (thread logic)
 *
 * This is where all the child threads will go
 * Make this a while loop to enure threads stay here and not escape
 */

void *thread_request_serve_static(void *arg)
{
  // For SFF
  int starvationSwitch = 0; // 0 is false, 1 is true
  int switched = 0; // 0 is false, 1 is true
  int starvationCounter = 0;
  while (1)
  {

    // Lock buffer
    pthread_mutex_lock(&buffer_lock);

    // Wait until there is a request
    while (buf_size == 0)
    {
      pthread_cond_wait(&buffer_not_empty, &buffer_lock);
    }

    bufferRequest request = {0, NULL, 0};

    // Fetch request from buffer and remove from buffer
    switch (scheduling_algo)
    {
    case (0):
      // First-in-first-out (FIFO): service the HTTP requests in the order they arrive.
      request = reqarr[0];
      // Shift requests in array to the left
      for (int index = 0; index < buf_size - 1; index++)
      {
        reqarr[index] = reqarr[index + 1];
      }
      buf_size--;
      break;
    case (1):
      // Smallest-file-first (SFF): service the HTTP requests by order of the requested file size. Starvation must be accounted for.

      if (starvationCounter > 100) {
        starvationSwitch = 1;
        // Reset counter
        // starvationCounter = 0;
      }

      // If switch is false continue with SFF
      if (starvationSwitch = 0)
      {
        int indexOfsmallestFile = 0;
        int sizeOfsmallestFile = 0;

        // Iterate through buffer
        for (int index = 0; index < buf_size - 1; index++)
        {
          int filesize = reqarr[index].pint_buf_size;
          if (filesize < sizeOfsmallestFile)
          {
            sizeOfsmallestFile = filesize;
            indexOfsmallestFile = index;
          }
        }

        request = reqarr[indexOfsmallestFile];
        // Shift requests in array to the left
        for (int index = indexOfsmallestFile; index < buf_size - 1; index++)
        {
          reqarr[index] = reqarr[index + 1];
        }
        buf_size--;
        starvationCounter++;
      }
      // Else switch was true so switch to FIFO mode to account for starvation
      else
      {
        // When our FIFO has ran for 100 requests switch back to SFF
        if (switched > 100)
        {
          starvationSwitch = 0;
          switched = 0;
          starvationCounter = 0;
        }

        // First-in-first-out (FIFO): service the HTTP requests in the order they arrive.
        request = reqarr[0];
        // Shift requests in array to the left
        for (int index = 0; index < buf_size - 1; index++)
        {
          reqarr[index] = reqarr[index + 1];
        }
        buf_size--;
      }

      break;

    case (2):
      // Random: service the HTTP requests by random order.

      int find = 0; // 0 is false, 1 is true.
      int index_to_remove;
      unsigned int seed = time(0);
      while (find == 0)
      {
        // Generate random index in buffer
        int rd_num = rand_r(&seed) % (buf_size - 0 + 1) + 0;
        // If our random index found a request at that index then stop
        if (reqarr[rd_num].filename != NULL)
        {
          find = 1;
          index_to_remove = rd_num;
          request = reqarr[index_to_remove];
        }
      }

      // Remove and shift requests in array to the left
      // Overwrite the request to remove
      for (int index = index_to_remove; index < buf_size - 1; index++)
      {
        reqarr[index] = reqarr[index + 1];
      }
      buf_size--;

      break;
    }

    pthread_cond_signal(&buffer_not_full);
    pthread_mutex_unlock(&buffer_lock);

    // Send to client
    request_serve_static(request.fd, request.filename, request.pint_buf_size);

    close_or_die(request.fd);
  }
}

//
// Initial handling of the request
//
void request_handle(int fd)
{
  int is_static;
  struct stat sbuf;
  char buf[MAXBUF], method[MAXBUF], uri[MAXBUF], version[MAXBUF];
  char filename[MAXBUF], cgiargs[MAXBUF];

  // get the request type, file path and HTTP version
  readline_or_die(fd, buf, MAXBUF);
  sscanf(buf, "%s %s %s", method, uri, version);
  printf("method:%s uri:%s version:%s\n", method, uri, version);

  // verify if the request type is GET or not
  if (strcasecmp(method, "GET"))
  {
    request_error(fd, method, "501", "Not Implemented", "server does not implement this method");
    return;
  }
  request_read_headers(fd);

  // check requested content type (static/dynamic)
  is_static = request_parse_uri(uri, filename, cgiargs);

  // get some data regarding the requested file, also check if requested file is present on server
  if (stat(filename, &sbuf) < 0)
  {
    request_error(fd, filename, "404", "Not found", "server could not find this file");
    return;
  }

  // verify if requested content is static
  if (is_static)
  {
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode))
    {
      request_error(fd, filename, "403", "Forbidden", "server could not read this file");
      return;
    }

    /**
     *
     * TODO: write code to add HTTP requests in the buffer based on the scheduling policy
     *
     * Based off arguments given in the server.c handle requests based off the following scheduling policies
     *
     **/

    // Check for directory traversal
    if (strstr(uri, "../"))
    {
      request_error(fd, filename, "404", "File not found", "no hacking the server today :) \nPath");
      return;
    }

    // Save request to struct before adding to buffer
    bufferRequest req = {fd, filename, sbuf.st_size};

    // Lock buffer
    pthread_mutex_lock(&buffer_lock);

    // Add request to buffer
    reqarr[buf_size] = req;
    buf_size++;

    // Signal our consumer/child threads
    pthread_cond_signal(&buffer_not_empty);
    // Unlock buffer
    pthread_mutex_unlock(&buffer_lock);
  }
  else
  {
    request_error(fd, filename, "501", "Not Implemented", "server does not serve dynamic content request");
  }
}
