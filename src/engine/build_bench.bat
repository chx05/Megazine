@g++ bench1.cpp -I. -L. -lntdll -lwinmm -pthread -std=c++20 -o bench1.exe -O3 -DNDEBUG
@g++ bench2a.cpp -I. -L. -lntdll -lwinmm -pthread -std=c++20 -o bench2a.exe -O3 -DNDEBUG
@g++ bench2b.cpp -I. -L. -lntdll -lwinmm -pthread -std=c++20 -o bench2b.exe -O3 -DNDEBUG