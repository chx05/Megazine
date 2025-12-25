#ifndef MGZ_LIB_C
#define MGZ_LIB_C

    #ifdef MGZ_LIBCLANG_ANALYSIS
        #define Clang_Note(n) __attribute__((annotate(n)))
    #else
        #define Clang_Note(n)
    #endif

    #define Macro_Tool_X_Str(x) #x
    #define Macro_Tool_Str(x) Macro_Tool_X_Str(x)

    #define Mgz_Root_Struct Clang_Note("Mgz_Root_Struct")
    #define Mgz_Len_Field(len_field_name) Clang_Note("Mgz_Len_Field:" Macro_Tool_Str(len_field_name))
    #define Mgz_Null_Ended Clang_Note("Mgz_Null_Ended")
    #define Mgz_Len_Const(len) Clang_Note("Mgz_Len_Const:" Macro_Tool_Str(len))
    #define Mgz_Len_Fn(fn_name) Clang_Note("Mgz_Len_Fn:" Macro_Tool_Str(fn_name))

#endif
