gcc -E -C -P "-I%(boost_root)s" "-D BOOST_USER_CONFIG=\"%(boost_root)s/libs/mpl/preprocessed/include/%(mode)s/user.hpp\"" -D BOOST_MPL_CFG_NO_OWN_PP_PRIMITIVES %(file)s >"%(file_path)s"
