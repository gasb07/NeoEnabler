#include <assert.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "../src/http_request.h"
static void check(const char*req,int good,const char*expected){
 int p[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,p));
 assert(write(p[0],req,strlen(req))==(ssize_t)strlen(req));close(p[0]);
 char buf[4096],*body=NULL;int rc=neo_http_read(p[1],buf,sizeof buf,&body);close(p[1]);
 assert((rc==0)==good);if(good)assert(!strcmp(body,expected));
}
int main(void){
 check("GET /gpu HTTP/1.1\r\nHost: console\r\n\r\n",1,"");
 check("POST /gpu/enable HTTP/1.1\r\nContent-Length: 8\r\n\r\nmhz=2350",1,"mhz=2350");
 check("POST /gpu/enable HTTP/1.1\r\ncontent-length: 8\r\n\r\nmhz=2350",1,"mhz=2350");
 check("POST / HTTP/1.1\r\nContent-Length: 8\r\n\r\nmhz=23",0,"");
 check("POST / HTTP/1.1\r\nContent-Length: 8\r\nContent-Length: 0\r\n\r\nmhz=2350",0,"");
 check("POST / HTTP/1.1\r\nContent-Length: 999999999999999999\r\n\r\n",0,"");
 check("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",0,"");
 check("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n",0,"");
 check("POST / HTTP/1.1\r\nContent-Length: 8x\r\n\r\nmhz=2350",0,"");
 int p[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,p));pid_t child=fork();assert(child>=0);
 if(!child){close(p[1]);const char*h="POST /gpu/enable HTTP/1.1\r\nContent-Length: 8\r\n\r\n";
  if(write(p[0],h,strlen(h))<0)_exit(1);
  usleep(10000);
  if(write(p[0],"mhz=2350",8)!=8)_exit(1);
  close(p[0]);_exit(0);
 }
 close(p[0]);char b[4096],*body=NULL;assert(!neo_http_read(p[1],b,sizeof b,&body)&&!strcmp(body,"mhz=2350"));
 close(p[1]);int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
 puts("PASS HTTP split POST body, malformed lengths, duplicate headers and bounded requests");return 0;
}
