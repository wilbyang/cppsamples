#include <string>
#include <iostream>

int main(int argc, char **argv) {
    auto f = []() { 
        std::string str = "1234567";
        return &str; // 返回临时变量的地址，此处为悬垂指针
    };
    
    auto s = f();
    std::cout<<*s<<"\n";
}