#include "logger.h"
#include <cassert>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
int main(int argc,char** argv) {
    assert(argc==2);setenv("WEBSERVER_LOG_FILE",argv[1],1);setenv("WEBSERVER_LOG_LEVEL","INFO",1);
    std::string message(1900,'x');
    errno=EAGAIN;
    for(int i=0;i<800;++i) LOG_INFO("test","event=rotation index=%d message=%s",i,message.c_str());
    assert(errno==EAGAIN);
    struct stat current={},rotated={};
    assert(stat(argv[1],&current)==0);
    assert(stat((std::string(argv[1])+".1").c_str(),&rotated)==0);
    assert(current.st_size>0 && current.st_size<1024*1024+4096);
    assert(rotated.st_size>=1024*1024 && rotated.st_size<1024*1024+4096);
}
