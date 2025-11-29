from clang.cindex import TranslationUnit
import fct

# CONFIGS for the source generator script
# /

from . import core

def execute(tu: TranslationUnit) -> list[fct.CppBuilder]:
    mg = core.MegazineGenerator(tu)
    mg.execute()
    return mg.sources
