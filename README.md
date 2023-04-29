# 基于epoll的多线程server

目前只支持Post与Get

下面是一个简单的示例

```c++
#include "libnet.h"
#define SERVER_PORT 53013
using libnet::Request;
using libnet::Response;
int main()
{
    libnet::Libnet net("0.0.0.0",SERVER_PORT);
    net.Post("/login", [](const Request & req, Response & res){
        for(const auto& i:req.params){
            std::cout<<i.first<<" "<<i.second<<"\n";
        }
        res.headers["Content-Type"]="text/json";
        res.params["code"]="1";
    });
    net.start();
}
```
```c++
//
// Created by gcw on 23-4-24.
//
#ifndef SHARE_STL_RPC_STL_H
#define SHARE_STL_RPC_STL_H
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/containers/map.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/interprocess/allocators/allocator.hpp>
#include <functional>
#include <utility>
#include <string>
#include <map>
#include <iostream>
namespace xiasui{
    using namespace boost::interprocess;
    using u_int=unsigned int;
    enum class option{create_only,open_only};

    template <typename KeyType,typename MappedType>
    class RPC_STL {
    public:
        typedef std::pair<const KeyType, MappedType> ValueType;
        typedef allocator<ValueType, managed_shared_memory::segment_manager> ShmemAllocator;
        typedef map<KeyType, MappedType, std::less<KeyType>, ShmemAllocator> MyMap;
        RPC_STL()=delete;

        RPC_STL(const create_only_t op,std::string name,u_int segment_memory,u_int max_massage_number):

                name_(std::move(name)),
                mq_(message_queue(create_only,name_.c_str(),max_massage_number,sizeof(u_int))),
                segment_(create_only,name.c_str(),segment_memory),
                alloc_inst_(segment_.get_segment_manager())
        {std::cout<<"3"<<std::endl;};
        RPC_STL(const open_only_t op,std::string name):
                name_(std::move(name)),
                mq_(message_queue(open_only,name_.c_str())),
                segment_(open_only,name.c_str()),
                alloc_inst_(segment_.get_segment_manager())
        {};
        void kill(){
            shared_memory_object::remove(name_.c_str());
            message_queue::remove(name_.c_str());
        }
        void send_map(std::map<KeyType,MappedType>&mp){
            MyMap* mymap=segment_.construct<MyMap>(std::to_string(++massage_id_).c_str())      //object name
                    (std::less<KeyType>() //first  ctor parameter
                            ,alloc_inst_);
            for(auto i:mp){
                mymap->insert(ValueType(i.first,i.second));
            }
            mq_.send(&massage_id_,sizeof (massage_id_),0);
        }
        MyMap* get(u_int& id){
            id=++received_id;
            return segment_.find<MyMap>(std::to_string(received_id).c_str()).first;
        }
        void destroy(u_int id){
            segment_.destroy<MyMap>(std::to_string(id).c_str());
        }
    private:
        std::string name_;
        message_queue mq_;
        managed_shared_memory segment_;
        ShmemAllocator alloc_inst_;
        u_int massage_id_{0};
        u_int received_id{0};
    };

    template <typename KeyType,typename MappedType>
    class stl_queue{
        typedef std::pair<const KeyType, MappedType> ValueType;
        typedef allocator<ValueType, managed_shared_memory::segment_manager> ShmemAllocator;
        typedef map<KeyType, MappedType, std::less<KeyType>, ShmemAllocator> MyMap;
    public:
        stl_queue(const option op,std::string name,int segment_memory=65536,int max_massage_number=10)
        {
            shared_memory_object::remove(name.c_str());
            std::cout<<"2"<<std::endl;
            if(op==option::create_only){
                rpc=new RPC_STL<KeyType,MappedType>(create_only,name,u_int(segment_memory),u_int(max_massage_number));
            }else if(op==option::open_only){
                rpc=new RPC_STL<KeyType,MappedType>(open_only,name);
            }
        }
        ~stl_queue(){
            rpc->kill();
            rpc= nullptr;
        }
        void send_map(std::map<KeyType,MappedType>&mp){
            rpc->send_map(mp);
        }
        MyMap* get(u_int &id){
            return rpc->get(id);
        }
        void destroy(u_int id){
            rpc->destroy(id);
        }
    private:
        RPC_STL<KeyType,MappedType>* rpc;
    };
}
#endif //SHARE_STL_RPC_STL_H

```
