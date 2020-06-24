
from libcpp cimport bool
from libcpp.string cimport string
from libcpp.map cimport map
from libcpp.vector cimport vector
from libcpp.memory cimport shared_ptr
from libcpp.utility cimport pair

from . cimport client
from .source cimport Source
from .sharedpv cimport SharedPV

cdef extern from "<pvxs/server.h>" namespace "pvxs::server" nogil:
    ctypedef map[string, string] Config_defs_t "pvxs::server::Config::defs_t"

    cdef cppclass Config:
        vector[string] interfaces
        vector[string] beaconDestinations
        unsigned short tcp_port
        unsigned short udp_port
        bool auto_beacon
        #std::array<uint8_t, 12> guid

        @staticmethod
        Config fromEnv() except+
        @staticmethod
        Config fromDefs(const map[string, string]&) except+
        @staticmethod
        Config isolated() except+

        Config& applyEnv() except+
        Config& applyDefs(const Config_defs_t&) except+
        void updateDefs(Config_defs_t& defs) except+

        void expand() except+
        Server build() except+

    cdef cppclass Server:
        Server()
        Server(const Config&) except+

        Server& start() except+
        Server& stop() except+
        Server& run() except+
        Server& interrupt() except+

        const Config& config() except+
        client.Config clientConfig() except+

        Server& addPV(const string& name, const SharedPV& pv) except+
        Server& removePV(const string& name) except+

        Server& addSource(const string& name, const shared_ptr[Source]& src, int order =0) except+
        shared_ptr[Source] removeSource(const string& name, int order =0) except+

        shared_ptr[Source] getSource(const string& name, int order =0) except+

        vector[pair[string, int]] listSource() except+
