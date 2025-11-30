from clang.cindex import TranslationUnit
import fct

# CONFIGS for the source generator script
GEN_FOR_CPP = True

from . import core

def execute(tu: TranslationUnit) -> list[fct.CppBuilder]:
    assert GEN_FOR_CPP, "Gen for C not supported yet"

    mg = core.MegazineGenerator(tu)
    mg.execute()
    return [mg.src]
