@g++ bench1.cpp -I. -L. -lmdbx -lntdll -lwinmm -pthread -o bench1.exe -O3 -DNDEBUG
@g++ bench2.cpp -I. -L. -lmdbx -lntdll -lwinmm -pthread -o bench2.exe -O3 -DNDEBUG