
from libcpp.memory cimport shared_ptr
from libcpp.string cimport string

from pvxs cimport client
from pvxs cimport source

cdef class ClientProvider:
    cdef client.Context ctxt

cdef class Source:
    cdef string name
    cdef shared_ptr[source.Source] src
