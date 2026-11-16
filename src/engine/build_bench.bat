@g++ bench1.cpp -I. -L. -lntdll -lwinmm -pthread -std=c++20 -o bench1.exe -O3 -DNDEBUG
@g++ bench2.cpp -I. -L. -lntdll -lwinmm -pthread -std=c++20 -o bench2.exe -O3 -DNDEBUG