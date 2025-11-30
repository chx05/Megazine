import fct

import megazine
import fct_toolbox
fct.install_tools(periodics=[megazine, fct_toolbox.struct_repr], manuals=[])
#fct.use_c_instead()
fct.run_argv()
