TEMPLATE = lib
TARGET = ublas

CONFIG += \
    staticlib \
    depend_includepath
CONFIG -= qt

INCLUDE_DIR = ../../../include

include(detail/detail.pri)
include(experimental/experimental.pri)
include(operation/operation.pri)
include(traits/traits.pri)

include(tensor/tensor.pri)

INCLUDEPATH += $${INCLUDE_DIR}

HEADERS += \
    $${INCLUDE_DIR}/boost/numeric/ublas/vector_sparse.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/vector_proxy.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/vector_of_vector.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/vector_expression.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/vector.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/triangular.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/traits.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/tags.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/symmetric.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/storage_sparse.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/storage.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/operation_sparse.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/operations.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/operation_blocked.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/operation.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/matrix_sparse.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/matrix_proxy.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/matrix_expression.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/matrix.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/lu.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/io.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/hermitian.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/fwd.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/functional.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/expression_types.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/exception.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/doxydoc.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/blas.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/banded.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/assignment.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/matrix_vector.hpp \
    $${INCLUDE_DIR}/boost/numeric/ublas/tensor.hpp
