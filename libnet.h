//
// Created by gcw on 23-2-12.
//

#ifndef JUDGER_LIBNET_H
#define JUDGER_LIBNET_H
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cassert>
#include <cstdio>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <cstdlib>
#include <sys/epoll.h>
#include <pthread.h>
#include <map>

#include <iostream>
#include <string>
#include <regex>
#include <utility>
#include <thread>
#include <condition_variable>
#include <list>
#include <queue>
#include <sstream>
namespace  libnet{

    struct Request {
        std::string method;//get还是post
        std::string url;

        std::unordered_map<std::string,std::string>  headers;//头
        std::unordered_map<std::string,std::string> params;
        bool has_headers(const std::string &key) const{
            return headers.count(key)!=0;
        };
        std::string get_headers(const std::string &key){
            return headers[key];
        };
        bool has_params(const std::string &key) const{
            return params.count(key)!=0;
        };
        std::string get_params(const std::string &key){
            return params[key];
        };
    };

    struct Response {
        std::unordered_map<std::string,std::string> headers;
        std::unordered_map<std::string,std::string> params;

        //注意返回的是一个引用
        const std::string& get_headers(const std::string &key){
            return headers[key];
        }

        const std::string& get_params(const std::string &key){
            return params[key];
        }

        bool has_params(const std::string &key) const{
            return params.count(key)!=0;
        }

        bool has_headers(const std::string &key)const {
            return headers.count(key)!=0;
        }

        //注意发送的时候要添加HTTP/1.1 200 OK Content-Length Content-Type Keep-Alive
        void init_res(){
            headers["Access-Control-Allow-Credentials"]="true";
            headers["Access-Control-Allow-Headers"]="Content-Type,Access-Control-Allow-Headers,Authorization,X-Requested-With";
            headers["Access-Control-Allow-Methods"]="GET, POST";
            headers["Access-Control-Allow-Origin"]="*";
            headers["Access-Control-Expose-Headers"]="content-type";
            headers["Keep-Alive"]="timeout=5, max=5";
        }
    };
    using Handler=std::function<void(const Request &,Response &)>;
    std::unordered_map<std::string,Handler>post_handler;//记录路径及其对应任务
    std::unordered_map<std::string,Handler>get_handler;//记录路径及其对应任务

    template<typename T>//线程安全队列
    class thread_safe_queue
    {
    private:
        mutable std::mutex mut;
        std::queue<T> data_queue;
        std::condition_variable data_cond;
    public:
        thread_safe_queue()
        {}
        void push(T new_value)
        {
            std::lock_guard<std::mutex> lk(mut);
            data_queue.push(std::move(new_value));
            data_cond.notify_one();
        }
        void wait_and_pop(T& value)
        {
            std::unique_lock<std::mutex> lk(mut);
            data_cond.wait(lk,[this]{return !data_queue.empty();});
            value=std::move(data_queue.front());
            data_queue.pop();
        }
        std::shared_ptr<T> wait_and_pop()
        {
            std::unique_lock<std::mutex> lk(mut);
            data_cond.wait(lk,[this]{return !data_queue.empty();});
            std::shared_ptr<T> res(
                    std::make_shared<T>(std::move(data_queue.front())));
            data_queue.pop();
            return res;
        }
        bool try_pop(T& value)
        {
            std::lock_guard<std::mutex> lk(mut);
            if(data_queue.empty())
                return false;
            value=std::move(data_queue.front());
            data_queue.pop();
            return true;
        }
        std::shared_ptr<T> try_pop()
        {
            std::lock_guard<std::mutex> lk(mut);
            if(data_queue.empty())
                return std::shared_ptr<T>();
            std::shared_ptr<T> res(
                    std::make_shared<T>(std::move(data_queue.front())));
            data_queue.pop();
            return res;
        }
        bool empty() const
        {
            std::lock_guard<std::mutex> lk(mut);
            return data_queue.empty();
        }
    };

    class join_threads
    {
        std::vector<std::thread>& threads;
    public:
        explicit join_threads(std::vector<std::thread>& threads_):threads(threads_)
        {}
        ~join_threads(){//析构前,先结束所有线程
            for(auto & thread : threads)
            {
                if(thread.joinable())
                    thread.join();
            }
        }
    };

    class thread_pool
    {
        std::atomic_bool done;
        thread_safe_queue<std::function<void()> > work_queue;//工作队列
        std::vector<std::thread> threads;//线程
        join_threads joiner;
        void worker_thread(){
            while(!done){
                std::function<void()> task;
                if(work_queue.try_pop(task)){
                    task();
                }
                else{
                    std::this_thread::yield();
                }
            }
        }
    public:
        thread_pool():done(false),joiner(threads){
            unsigned const thread_count=std::thread::hardware_concurrency();//获取核的数量
            try{
                for(unsigned i=0;i<thread_count;++i){
                    threads.emplace_back(&thread_pool::worker_thread,this);
                }
            }
            catch(...){
                done=true;
                throw;
            }
        }
        ~thread_pool(){
            done=true;
        }
        template<typename FunctionType>
        void submit(FunctionType f){
            work_queue.push(std::function<void()>(f));
        }
    };
    struct Work {
        Request req;
        Response res;
        int fd;
        explicit Work(int fd1):fd(fd1),req(),res(){//构造时读取数据
            std::string line;
            char buf[1024];
            while(true) {
                int idx = 0;//当前解析位置
                memset(buf, 0, 10);
                int ret = int(recv(this->fd, buf, 10 - 1, 0));
                if(ret < 0){
                    /*对于非阻塞IO，下面的事件成立标识数据已经全部读取完毕。此后，epoll就能再次触发sockfd上的sockfd的EPOLLIN事件，以驱动下一次读操作*/
                    if((errno == EAGAIN) || (errno == EWOULDBLOCK)){
                        break;
                    }
                    close(fd);
                    break;
                }else if(ret == 0){
                    close(fd);
                }else{
                    line+=buf;
                }
            }
            std::string tmp;//头部里别放换行符
            bool first_get=true;
            int idx=0;
            for(;idx<line.size();idx++){
                auto i=line[idx];
                if(i=='\n'){
                    if(tmp.size()<=1)break;
                    if(first_get){
                        first_get=false;
                        bool ok=false;
                        for(const auto j:line){
                            if(j==' '){
                                if(ok)break;
                                else ok=true;
                            }else{
                                if(!ok)req.method+=j;
                                else req.url+=j;
                            }
                        }
                    }else{
                        std::string tmp_key;
                        int j=0;
                        for(j=0;j<tmp.size();j++){
                            if(tmp[j]==':'){
                                break;
                            }
                            tmp_key+=tmp[j];
                        }
                        req.headers[tmp_key]="";
                        auto it=req.headers.find(tmp_key);
                        for(j++;j<tmp.size();j++){
                            it->second+=tmp[j];
                        }
                    }
                    tmp.clear();
                }else{
                    tmp+=i;
                }
            }
            tmp.clear();
            std::string encode_str;
            auto FromHex=[](unsigned char x)->unsigned char{
                unsigned char y;
                if (x >= 'A' && x <= 'Z') y = x - 'A' + 10;
                else if (x >= 'a' && x <= 'z') y = x - 'a' + 10;
                else if (x >= '0' && x <= '9') y = x - '0';
                else return 0;
                return y;
            };
            //对url编码进行解码
            for(++idx;idx<line.size();++idx){
                if (line[idx] == '+') {
                    encode_str += ' ';
                } else if (idx + 2 < line.size() && line[idx] == '%') {
                    if(line[idx+1]=='3'&&line[idx+2]=='D'||
                            line[idx+1]=='2'&&line[idx+2]=='6'||
                            line[idx+1]=='2'&&line[idx+2]=='5')
                    {
                        encode_str+=line[idx];
                        encode_str+=line[idx+1];
                        encode_str+=line[idx+2];
                        idx+=2;
                        continue;
                    }
                    unsigned char high = FromHex((unsigned char)line[++idx]);
                    unsigned char low = FromHex((unsigned char)line[++idx]);
                    encode_str += high*16 + low;
                } else {
                    encode_str += line[idx];
                }
            }
            bool is_key=true;
            std::unordered_map<std::string,std::string>::iterator it;
            for (idx=0;idx<encode_str.size();++idx){
                if(encode_str[idx]=='%'){//转义字符
                    char trans=' ';
                    if(encode_str[idx+1]=='2'&&encode_str[idx+2]=='6')trans='$';
                    else if(encode_str[idx+1]=='3'&&encode_str[idx+2]=='D')trans='=';
                    else if(encode_str[idx+1]=='2'&&encode_str[idx+2]=='5')trans='%';
                    if(is_key)tmp+=trans;
                    else it->second+=trans;
                    idx+=2;
                }else if(encode_str[idx]=='='){//真的
                    is_key=false;
                    req.params[tmp]="";
                    it=req.params.find(tmp);
                }else if(encode_str[idx]=='&'){
                    tmp.clear();
                    is_key=true;
                }else{
                    if(is_key)tmp+=encode_str[idx];
                    else it->second+=encode_str[idx];
                }
            }
            res.init_res();
            if(req.method=="POST"){
                if(post_handler.count(req.url)!=0){
                    auto fn=post_handler[req.url];
                    fn(req,res);
                }
            }else if(req.method=="GET"){
                if(get_handler.count(req.url)!=0){
                    auto fn=post_handler[req.url];
                    fn(req,res);
                }
            }
            std::cout<<"work_down\n";
        }
        ~Work(){
            std::string res_data("HTTP/1.1 200 OK\r\n");
            int params_size=3+res.params.size()*4+res.params.size()-1;
            for(const auto& i:res.params){
                params_size+=i.first.size()+i.second.size();
            }
            for(const auto& i:res.headers){
                res_data+=(i.first+":"+i.second+"\r\n");
            }
            res_data+="Content-Length:"+ std::to_string(params_size)+"\r\n\r\n";
            res_data+="{";
            for(const auto &i:res.params){
                res_data+="\""+i.first+"\":\""+i.second+"\",";
            }
            if(!res.params.empty())
                res_data.pop_back();
            res_data+="}";

            std::cout<<res_data<<"\n";
            std::cout<<send(this->fd,res_data.c_str(),res_data.size(),0);
            close(fd);
        }
    };




#define MAX_EVENT_NUMBER 100
#define BUFFER_SIZE 10
    class Libnet{
    private:
        thread_pool task_queue;//任务队列
        std::string ip{};
        int port{};
        int epoll_fd{};
        int listen_fd{};
        /*将文件描述符设置为非阻塞*/
        int setnonblocking(int fd){
            int old_option = fcntl(fd, F_GETFL);
            int new_option = old_option | O_NONBLOCK;
            fcntl(fd, F_SETFL, new_option);
            return old_option;
        }
        /*将文件描述符fd上的EPOLLIN注册到epollfd指示的epoll内核事件表中。 参数enable_et 指定是否对fd采用ET模式*/
        void addfd(int epollfd1, int fd1, int enable_et)
        {
            struct epoll_event event{};
            event.data.fd = fd1;
            event.events  = EPOLLIN;
            if(enable_et){
                event.events |= EPOLLET;
            }
            epoll_ctl(epollfd1, EPOLL_CTL_ADD, fd1, &event );
            setnonblocking(fd1);
        }
        void et(struct epoll_event* events, int number, int epollfd, int listenfd)
        {
            char buf[BUFFER_SIZE];
            int i=0;
            for(i =0; i<number; i++){
                int sockfd = events[i].data.fd;
                if(sockfd == listenfd){
                    struct sockaddr_in client_address{};
                    socklen_t client_addrlength = sizeof(client_address);
                    int connfd = accept(listenfd, (struct sockaddr* )&client_address, &client_addrlength);
                    addfd(epollfd, connfd, true);/*对connfd开启ET模式*/
                }else if(events[i].events & EPOLLIN){// 数据可读
                    /*这段代码不会被重复触发，所以我们循环读取数据，以确保把socket缓冲区的数据全部读取*/
                    task_queue.submit([sockfd](){
                        {
                            Work w(sockfd);
                        }
                    });
                }else{
                    printf("something else happen\n");
                }

            }

        }
    public:
        Libnet(std::string _ip,int _port):ip(std::move(_ip)),port(_port), task_queue(){

        };
        ~Libnet() {
            close(this->listen_fd);
            close(this->epoll_fd);
        };
        bool start(){
            struct sockaddr_in address{};
            bzero(&address,sizeof address);
            address.sin_family=AF_INET;
            inet_pton(AF_INET, this->ip.c_str(),&address.sin_addr);
            address.sin_port= htons(port);
            this->listen_fd = socket(PF_INET, SOCK_STREAM, 0);
            if(this->listen_fd<0)
                return false;
            int opt=1;
            setsockopt(this->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            if(bind(this->listen_fd,(struct sockaddr*)&address,sizeof address)<0){
                close(this->listen_fd);
                return false;
            }
            if(listen(this->listen_fd,10)<0){
                close(this->listen_fd);
                return false;
            }
            struct epoll_event events[MAX_EVENT_NUMBER];
            this->epoll_fd=epoll_create(50);
            if(this->epoll_fd<0){
                close(epoll_fd);
                close(this->listen_fd);
                return false;
            }
            addfd(this->epoll_fd, this->listen_fd,true);
            while(true){
                int ret = epoll_wait(epoll_fd, events, MAX_EVENT_NUMBER, -1);
                if(ret<0){
                    std::cerr<<"epoll error\n";
                    break;
                }
                et(events, ret, epoll_fd, this->listen_fd);
            }
            return true;
        }
        void Get(const std::string &pattern,Handler handler){
            get_handler[pattern]=std::move(handler);
        }
        void Post(const std::string &pattern,Handler handler){
            post_handler[pattern]=std::move(handler);
        }


    };
#undef MAX_EVENT_NUMBER
#undef BUFFER_SIZE
}


#endif //JUDGER_LIBNET_H
