// Stands in for liblsfg-vk-layer.so: a library using the system libstdc++.so for stream parsing.
#include <cstdio>
#include <sstream>

extern "C" void layer_parse() {
    std::stringstream ss("1.25");
    double v = 0;
    ss >> v;
    printf("[layer] parsed %.2f\n", v);
}
