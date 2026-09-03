#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>

int main() {
    iobject::IRuntimeObject* node = iobject::Runtime::make();
    TEST_CHECK(node != nullptr);
    delete node;
    return 0;
}
