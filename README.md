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

