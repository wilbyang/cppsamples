#include <iostream>
#include <cstddef> // 用于 size_t

// --- 策略 1: 一个空的删除器 ---
// 它只有行为 (operator())，没有数据成员。
struct EmptyDeleter {
    void operator()(int* p) const {
        std::cout << "  [EmptyDeleter 策略]: 正在删除指针...\n";
        delete p;
    }
};

// --- 策略 2: 一个非空的删除器 (用于对比) ---
// 它有数据成员，所以它不是空的。
struct NonEmptyDeleter {
    int delete_count = 0; // 占用空间的数据成员

    void operator()(int* p) {
        delete_count++;
        std::cout << "  [NonEmptyDeleter 策略]: 正在删除指针... (第 " << delete_count << " 次)\n";
        delete p;
    }
};


// --- 方案 A: 失败的行动 (使用 "组合") ---
// 将策略作为成员变量
template <typename T, typename Deleter>
class MySmartPtr_Naive {
public:
    // 构造函数：获取原始指针
    explicit MySmartPtr_Naive(T* ptr) : m_ptr(ptr) {} // m_del 被默认构造

    // 析构函数：使用成员 m_del 来释放指针
    ~MySmartPtr_Naive() {
        if (m_ptr) {
            m_del(m_ptr);
        }
    }

private:
    T* m_ptr;       // 成员1: 原始指针
    Deleter m_del;  // 成员2: 策略对象
};


// --- 方案 B: 成功的行动 (使用 "继承") ---
// 从策略类私有继承
template <typename T, typename Deleter>
class MySmartPtr_EBO : private Deleter { // 重点：私有继承
public:
    // 构造函数：获取原始指针 (Deleter 基类被默认构造)
    explicit MySmartPtr_EBO(T* ptr) : m_ptr(ptr) {}

    // 析构函数：调用 *this (即基类) 的 operator()
    ~MySmartPtr_EBO() {
        if (m_ptr) {
            (*this)(m_ptr); // "调用" 基类
        }
    }

private:
    T* m_ptr; // 唯一的成员变量
};


int main() {
    std::cout << "--- 编译器基准 (64位系统) ---\n";
    std::cout << "sizeof(int*):         " << sizeof(int*) << " 字节 (这是我们的基准)\n";
    std::cout << "sizeof(EmptyDeleter):   " << sizeof(EmptyDeleter) << " 字节 (空类至少为 1)\n";
    std::cout << "sizeof(NonEmptyDeleter): " << sizeof(NonEmptyDeleter) << " 字节 (因为它有一个 int)\n";

    std::cout << "\n--- 测试: 使用空策略 (EmptyDeleter) ---\n";
    std::cout << "Naive (组合) 大小: " << sizeof(MySmartPtr_Naive<int, EmptyDeleter>)
              << " 字节 (8 + 1 + 7字节填充 = 16)\n";
    std::cout << "EBO (继承) 大小:   " << sizeof(MySmartPtr_EBO<int, EmptyDeleter>)
              << " 字节 (只有指针的 8 字节!)\n";

    std::cout << "\n--- 测试: 使用非空策略 (NonEmptyDeleter) ---\n";
    std::cout << "Naive (组合) 大小: " << sizeof(MySmartPtr_Naive<int, NonEmptyDeleter>)
              << " 字节 (8 + 4 + 4字节填充 = 16)\n";
    std::cout << "EBO (继承) 大小:   " << sizeof(MySmartPtr_EBO<int, NonEmptyDeleter>)
              << " 字节 (8 + 4 + 4字节填充 = 16)\n";

    std::cout << "\n--- 功能测试 (EBO 版本) ---\n";
    { // 创建一个新的作用域
        MySmartPtr_EBO<int, EmptyDeleter> ptr(new int(123));
        std::cout << "  创建了 ptr, 即将离开作用域...\n";
    } // ptr 在这里被析构，自动调用 EmptyDeleter
    std::cout << "  已离开作用域。\n";

    return 0;
}