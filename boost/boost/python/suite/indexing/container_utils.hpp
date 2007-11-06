
//  (C) Copyright Joel de Guzman 2003.
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

#ifndef PY_CONTAINER_UTILS_JDG20038_HPP
# define PY_CONTAINER_UTILS_JDG20038_HPP

# include <boost/python/object.hpp>
# include <boost/python/handle.hpp>
# include <boost/python/extract.hpp>

namespace boost { namespace python { namespace container_utils {
        
    template <typename Container>
    void
    extend_container(Container& container, object l)
    {
        typedef typename Container::value_type data_type;
        
        //  l must be a list or some container

        for (int i = 0; i < l.attr("__len__")(); i++)
        {
            object elem(l[i]);
            extract<data_type const&> x(elem);
            //  try if elem is an exact data_type type
            if (x.check())
            {
                container.push_back(x());
            }
            else
            {
                //  try to convert elem to data_type type
                extract<data_type> x(elem);
                if (x.check())
                {
                    container.push_back(x());
                }
                else
                {
                    PyErr_SetString(PyExc_TypeError, "Incompatible Data Type");
                    throw_error_already_set();
                }
            }
        }          
    }

}}} // namespace boost::python::container_utils

#endif
