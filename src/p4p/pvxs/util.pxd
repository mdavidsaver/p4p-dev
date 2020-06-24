#cython: language_level=2

from libcpp.string cimport string
from libcpp.map cimport map

cdef extern from "<pvxs/util.h>" namespace "pvxs" nogil:
    map[string, size_t] instanceSnapshot() except+
