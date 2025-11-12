#include <cstdio>
#include <memory>
#include <utility>
#include <vector>


class Drawable {
public:
    void draw() const { ptr_->draw(); }
    template<class T>
    Drawable(T&& obj)
        : ptr_(std::make_shared<Model<T>>(std::forward<T>(obj))) {}
private:
    struct Concept {
        virtual ~Concept() = default;
        virtual void draw() const = 0;
    };
    template<class T>
    struct Model final : Concept {
        T obj_;
        explicit Model(T&& o) : obj_(std::move(o)) {}
        void draw() const override { obj_.draw(); }
    };
    std::shared_ptr<const Concept> ptr_;
};


struct Circle { void draw() const { std::puts("○"); } };
struct Square { void draw() const { std::puts("□"); } };

int main() {
    std::vector<Drawable> v{ Circle{}, Square{} };
    for (auto& d : v) d.draw();
}
// 场景：运行期多态（动态分发）
// 目标：值语义、无裸指针、可序列化、可反射
// 技法：Type Erasure + std::any 内核 + 小对象优化