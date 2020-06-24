
from libcpp cimport bool
from libcpp.string cimport string
from libcpp.memory cimport shared_ptr

from .data cimport Value

cdef extern from "<pvxs/source.h>" namespace "pvxs::server" nogil:
    cdef cppclass Source:
        pass

    enum op_t "pvxs::server::OpBase::op_t":
        None "pvxs::server::OpBase::None",
        Info "pvxs::server::OpBase::Info",
        Get "pvxs::server::OpBase::Get",
        Put "pvxs::server::OpBase::Put",
        RPC "pvxs::server::OpBase::RPC",

    cdef cppclass OpBase:
        const string& peerName() const
        const string& ifaceName() const
        const string& name() const

        #std::pair<std::string, Value> rawCredentials() const

    cdef cppclass ExecOp(OpBase):
        void reply() except+
        void reply(const Value&) except+
        void error(const string&) except+
