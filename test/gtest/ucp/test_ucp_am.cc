/**
 * Copyright (C) Mellanox Technologies Ltd. 2001-2015.  ALL RIGHTS RESERVED.
 * Copyright (c) UT-Battelle, LLC. 2015. ALL RIGHTS RESERVED.
 * Copyright (C) Los Alamos National Security, LLC. 2018. ALL RIGHTS RESERVED.
 *
 */
#include <list>
#include <numeric>
#include <set>
#include <vector>
#include <math.h>

#include <common/test.h>

#include "ucp_datatype.h"
#include "ucp_test.h"

extern "C" {
#include <ucp/core/ucp_am.h>
#include <ucp/core/ucp_ep.inl>
}

#define NUM_MESSAGES 17

#define UCP_REALLOC_ID 1000
#define UCP_SEND_ID 0
#define UCP_REPLY_ID 1
#define UCP_RELEASE 1

class test_ucp_am_base : public ucp_test {
public:
    static ucp_params_t get_ctx_params() {
        ucp_params_t params = ucp_test::get_ctx_params();
        params.field_mask  |= UCP_PARAM_FIELD_FEATURES;
        params.features     = UCP_FEATURE_AM;
        return params;
    }

    virtual void init() {
        modify_config("MAX_EAGER_LANES", "2");

        ucp_test::init();
        sender().connect(&receiver(), get_ep_params());
        receiver().connect(&sender(), get_ep_params());
    }
};

class test_ucp_am : public test_ucp_am_base {
public:
    int sent_ams;
    int replies;
    int recv_ams;
    void *reply;
    void *for_release[NUM_MESSAGES];
    int release;

    static ucs_status_t ucp_process_am_cb(void *arg, void *data,
                                          size_t length,
                                          ucp_ep_h reply_ep,
                                          unsigned flags);

    static ucs_status_t ucp_process_reply_cb(void *arg, void *data,
                                             size_t length,
                                             ucp_ep_h reply_ep,
                                             unsigned flags);

    ucs_status_t am_handler(test_ucp_am *me, void *data,
                            size_t  length, unsigned flags);

protected:
    void do_set_am_handler_realloc_test();
    void do_send_process_data_test(int test_release, uint16_t am_id,
                                   int send_reply);
    void do_send_process_data_iov_test(size_t size);
    void set_handlers(uint16_t am_id);
    void set_reply_handlers();
};

ucs_status_t test_ucp_am::ucp_process_reply_cb(void *arg, void *data,
                                               size_t length,
                                               ucp_ep_h reply_ep,
                                               unsigned flags)
{
    test_ucp_am *self = reinterpret_cast<test_ucp_am*>(arg);
    self->replies++;
    return UCS_OK;
}

ucs_status_t test_ucp_am::ucp_process_am_cb(void *arg, void *data,
                                            size_t length,
                                            ucp_ep_h reply_ep,
                                            unsigned flags)
{
    test_ucp_am *self = reinterpret_cast<test_ucp_am*>(arg);

    if (reply_ep) {
        self->reply = ucp_am_send_nb(reply_ep, UCP_REPLY_ID, NULL, 1,
                                     ucp_dt_make_contig(0),
                                     (ucp_send_callback_t) ucs_empty_function,
                                     0);
        EXPECT_FALSE(UCS_PTR_IS_ERR(self->reply));
    }

    return self->am_handler(self, data, length, flags);
}

ucs_status_t test_ucp_am::am_handler(test_ucp_am *me, void *data,
                                     size_t length, unsigned flags)
{
    ucs_status_t status;
    std::vector<char> cmp(length, (char)length);
    std::vector<char> databuf(length, 'r');

    memcpy(&databuf[0], data, length);

    EXPECT_EQ(cmp, databuf);

    bool has_desc = flags & UCP_CB_PARAM_FLAG_DATA;
    if (me->release) {
        me->for_release[me->recv_ams] = has_desc ? data : NULL;
        status                        = has_desc ? UCS_INPROGRESS : UCS_OK;
    } else {
        status                        = UCS_OK;
    }

    me->recv_ams++;
    return status;
}


void test_ucp_am::set_reply_handlers()
{
    ucp_worker_set_am_handler(sender().worker(), UCP_REPLY_ID,
                              ucp_process_reply_cb, this,
                              UCP_AM_FLAG_WHOLE_MSG);
    ucp_worker_set_am_handler(receiver().worker(), UCP_REPLY_ID,
                              ucp_process_reply_cb, this,
                              UCP_AM_FLAG_WHOLE_MSG);
}

void test_ucp_am::set_handlers(uint16_t am_id)
{
    ucp_worker_set_am_handler(sender().worker(), am_id,
                              ucp_process_am_cb, this,
                              UCP_AM_FLAG_WHOLE_MSG);
    ucp_worker_set_am_handler(receiver().worker(), am_id,
                              ucp_process_am_cb, this,
                              UCP_AM_FLAG_WHOLE_MSG);
}

void test_ucp_am::do_send_process_data_test(int test_release, uint16_t am_id,
                                            int send_reply)
{
    size_t buf_size          = pow(2, NUM_MESSAGES - 2);
    ucs_status_ptr_t sstatus = NULL;
    std::vector<char> buf(buf_size);

    recv_ams      = 0;
    sent_ams      = 0;
    replies       = 0;
    this->release = test_release;

    for (size_t i = 0; i < buf_size + 1; i = i ? (i * 2) : 1) {
        for (size_t j = 0; j < i; j++) {
            buf[j] = i;
        }

        reply   = NULL;
        sstatus = ucp_am_send_nb(sender().ep(), am_id,
                                 buf.data(), 1, ucp_dt_make_contig(i),
                                 (ucp_send_callback_t) ucs_empty_function,
                                 send_reply);

        EXPECT_FALSE(UCS_PTR_IS_ERR(sstatus));
        request_wait(sstatus);
        sent_ams++;

        if (send_reply) {
            while (sent_ams != replies) {
                progress();
            }

            if (reply != NULL) {
                ucp_request_release(reply);
            }
        }
    }

    while (sent_ams != recv_ams) {
        progress();
    }

    if (send_reply) {
        while (sent_ams != replies) {
            progress();
        }
    }

    if (test_release) {
        for(int i = 0; i < recv_ams; i++) {
            if (for_release[i] != NULL) {
                ucp_am_data_release(receiver().worker(), for_release[i]);
            }
        }
    }
}

void test_ucp_am::do_send_process_data_iov_test(size_t size)
{
    ucs_status_ptr_t sstatus;
    size_t index;
    size_t i;

    recv_ams = 0;
    sent_ams = 0;
    release  = 0;

    const size_t iovcnt = 2;
    std::vector<char> sendbuf(size * iovcnt, 0);

    ucs::fill_random(sendbuf);

    set_handlers(0);

    for (i = 1; i < size; i *= 2) {
        for (size_t iov_it = 0; iov_it < iovcnt; iov_it++) {
            for (index = 0; index < i; index++) {
                sendbuf[(iov_it * i) + index] = i * 2;
            }
        }

        ucp::data_type_desc_t send_dt_desc(DATATYPE_IOV, sendbuf.data(),
                                           i * iovcnt, iovcnt);

        sstatus = ucp_am_send_nb(sender().ep(), 0,
                                 send_dt_desc.buf(), iovcnt, DATATYPE_IOV,
                                 (ucp_send_callback_t) ucs_empty_function, 0);
        request_wait(sstatus);
        EXPECT_FALSE(UCS_PTR_IS_ERR(sstatus));
        sent_ams++;
    }

    while (sent_ams != recv_ams) {
        progress();
    }
}

void test_ucp_am::do_set_am_handler_realloc_test()
{
    set_handlers(UCP_SEND_ID);
    do_send_process_data_test(0, UCP_SEND_ID, 0);

    set_handlers(UCP_REALLOC_ID);
    do_send_process_data_test(0, UCP_REALLOC_ID, 0);

    set_handlers(UCP_SEND_ID + 1);
    do_send_process_data_test(0, UCP_SEND_ID + 1, 0);
}

UCS_TEST_P(test_ucp_am, send_process_am, "RNDV_THRESH=-1")
{
    set_handlers(UCP_SEND_ID);
    do_send_process_data_test(0, UCP_SEND_ID, 0);

    set_reply_handlers();
    do_send_process_data_test(0, UCP_SEND_ID, UCP_AM_SEND_REPLY);
}

UCS_TEST_P(test_ucp_am, send_process_am_release, "RNDV_THRESH=-1")
{
    set_handlers(UCP_SEND_ID);
    do_send_process_data_test(UCP_RELEASE, 0, 0);
}

UCS_TEST_P(test_ucp_am, send_process_iov_am, "RNDV_THRESH=-1")
{
    ucs::detail::message_stream ms("INFO");

    for (unsigned i = 1; i <= 7; ++i) {
        size_t max = (long)pow(10.0, i);
        long count = ucs_max((long)(5000.0 / sqrt(max) /
                                    ucs::test_time_multiplier()), 3);
        ms << count << "x10^" << i << " " << std::flush;
        for (long j = 0; j < count; ++j) {
            size_t size = ucs::rand() % max + 1;
            do_send_process_data_iov_test(size);
        }
    }
}

UCS_TEST_P(test_ucp_am, set_am_handler_realloc, "RNDV_THRESH=-1")
{
    do_set_am_handler_realloc_test();
}

UCP_INSTANTIATE_TEST_CASE(test_ucp_am)


class test_ucp_am_nbx : public test_ucp_am_base {
public:
    enum {
        DT_CONTIG  = UCS_BIT(0),
        DT_IOV     = UCS_BIT(1),
        DT_GENERIC = UCS_BIT(2),
        DT_NUM     = 3,
        REPLY_FLAG = UCS_BIT(3)
    };

    test_ucp_am_nbx()
    {
        m_dt          = ucp_dt_make_contig(1);
        m_am_received = false;
    }

    size_t max_am_hdr()
    {
        ucp_worker_attr_t attr;
        attr.field_mask = UCP_WORKER_ATTR_FIELD_MAX_AM_HEADER;

        ASSERT_UCS_OK(ucp_worker_query(sender().worker(), &attr));
        return attr.max_am_header;
    }

    virtual unsigned get_send_flag()
    {
        return 0;
    }

    ucp_datatype_t make_dt(int dt)
    {
        if (dt & DT_CONTIG) {
           return ucp_dt_make_contig(1);
        } else if (dt & DT_IOV) {
           return ucp_dt_make_iov();
        } else {
            EXPECT_TRUE(dt & DT_GENERIC);

            ucp_datatype_t ucp_dt;
            ASSERT_UCS_OK(ucp_dt_create_generic(&ucp::test_dt_copy_ops, NULL,
                                                &ucp_dt));
            return ucp_dt;
        }
    }

    void destroy_dt(ucp_datatype_t dt)
    {
        if (UCP_DT_IS_GENERIC(dt)) {
            ucp_dt_destroy(dt);
        }
    }

    void set_am_data_handler(entity &e, uint16_t am_id,
                             ucp_am_recv_callback_t cb, void *arg)
    {
        ucp_am_handler_param_t param;

        /* Initialize Active Message data handler */
        param.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID |
                           UCP_AM_HANDLER_PARAM_FIELD_CB |
                           UCP_AM_HANDLER_PARAM_FIELD_ARG;
        param.id         = am_id;
        param.cb         = cb;
        param.arg        = arg;
        ASSERT_UCS_OK(ucp_worker_set_am_recv_handler(e.worker(), &param));
    }

    ucs_status_ptr_t send_am(const ucp::data_type_desc_t& dt_desc,
                             unsigned flags = 0, const void *hdr = NULL,
                             unsigned hdr_length = 0)
    {
        ucp_request_param_t param;
        param.op_attr_mask      = UCP_OP_ATTR_FIELD_DATATYPE;
        param.datatype          = dt_desc.dt();

        if (flags != 0) {
            param.op_attr_mask |= UCP_OP_ATTR_FIELD_FLAGS;
            param.flags         = flags;
        }

        ucs_status_ptr_t sptr = ucp_am_send_nbx(sender().ep(), TEST_AM_NBX_ID,
                                                hdr, hdr_length, dt_desc.buf(),
                                                dt_desc.count(), &param);
        return sptr;
    }

    void test_am_send_recv(size_t size, size_t header_size = 0ul,
                           unsigned flags = 0, bool hold_desc = false)
    {
        std::string sbuf(size, 'd');
        std::string hbuf(header_size, 'h');
        m_am_received = false;

        set_am_data_handler(receiver(), TEST_AM_NBX_ID, am_data_cb, this);

        ucp::data_type_desc_t sdt_desc(m_dt, &sbuf[0], size);

        ucs_status_ptr_t sptr = send_am(sdt_desc, get_send_flag(),
                                        hbuf.c_str(), header_size);

        wait_for_flag(&m_am_received);
        request_wait(sptr);
        EXPECT_TRUE(m_am_received);
    }

    void test_am(size_t size)
    {
        size_t small_hdr_size = 8;

        test_am_send_recv(size, small_hdr_size);
        test_am_send_recv(size, 0);

        if (max_am_hdr() > small_hdr_size) {
            test_am_send_recv(size, max_am_hdr());
        }
    }

    virtual ucs_status_t am_data_handler(const void *header,
                                         size_t header_length,
                                         void *data, size_t length,
                                         const ucp_am_recv_param_t *rx_param)
    {
        EXPECT_FALSE(m_am_received);
        EXPECT_EQ(std::string::npos,
                  std::string((const char*)data, length).find_first_not_of('d'));

        if (header_length != 0) {
            EXPECT_EQ(std::string::npos,
                      std::string((const char*)header,
                                  header_length).find_first_not_of('h'));
        }

        bool has_reply_ep = get_send_flag();

        EXPECT_EQ(has_reply_ep, rx_param->recv_attr &
                                UCP_AM_RECV_ATTR_FIELD_REPLY_EP);
        EXPECT_EQ(has_reply_ep, rx_param->reply_ep != NULL);

        EXPECT_FALSE(rx_param->recv_attr & UCP_AM_RECV_ATTR_FLAG_RNDV);

        m_am_received = true;

        return UCS_OK;
    }

    static ucs_status_t am_data_cb(void *arg, const void *header,
                                   size_t header_length, void *data,
                                   size_t length,
                                   const ucp_am_recv_param_t *param)
    {
        test_ucp_am_nbx *self = reinterpret_cast<test_ucp_am_nbx*>(arg);
        return self->am_data_handler(header, header_length, data, length, param);
    }

    static const uint16_t           TEST_AM_NBX_ID = 0;
    ucp_datatype_t                  m_dt;
    volatile bool                   m_am_received;
};

UCS_TEST_P(test_ucp_am_nbx, set_invalid_handler)
{
    ucp_am_handler_param_t params;

    params.id           = 0;
    params.cb           = NULL;
    params.field_mask   = UCP_AM_HANDLER_PARAM_FIELD_ID |
                          UCP_AM_HANDLER_PARAM_FIELD_CB;
    ucs_status_t status = ucp_worker_set_am_recv_handler(sender().worker(),
                                                         &params);
    EXPECT_UCS_OK(status);

    // Check that error is returned if id or callback is not set
    scoped_log_handler wrap_err(wrap_errors_logger);

    params.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID;
    status            = ucp_worker_set_am_recv_handler(sender().worker(),
                                                       &params);
    EXPECT_EQ(UCS_ERR_INVALID_PARAM, status);

    params.field_mask = UCP_AM_HANDLER_PARAM_FIELD_CB;
    status            = ucp_worker_set_am_recv_handler(sender().worker(),
                                                       &params);
    EXPECT_EQ(UCS_ERR_INVALID_PARAM, status);

    params.field_mask = 0ul;
    status            = ucp_worker_set_am_recv_handler(sender().worker(),
                                                       &params);
    EXPECT_EQ(UCS_ERR_INVALID_PARAM, status);

    // Check that error is returned if private flag is requested by the user
    params.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID |
                        UCP_AM_HANDLER_PARAM_FIELD_CB |
                        UCP_AM_HANDLER_PARAM_FIELD_FLAGS;
    params.flags      = UCP_AM_CB_PRIV_FLAG_NBX;
    status            = ucp_worker_set_am_recv_handler(sender().worker(),
                                                       &params);
    EXPECT_EQ(UCS_ERR_INVALID_PARAM, status);
}

UCS_TEST_P(test_ucp_am_nbx, max_am_header)
{
    size_t min_am_bcopy       = std::numeric_limits<size_t>::max();
    bool has_tl_with_am_bcopy = false;

    for (ucp_rsc_index_t idx = 0; idx < sender().ucph()->num_tls; ++idx) {
        uct_iface_attr_t *attr = ucp_worker_iface_get_attr(sender().worker(), idx);
        if (attr->cap.flags & UCT_IFACE_FLAG_AM_BCOPY) {
            min_am_bcopy = ucs_min(min_am_bcopy, attr->cap.am.max_bcopy);
            has_tl_with_am_bcopy = true;
        }
    }

    EXPECT_TRUE(has_tl_with_am_bcopy);

    EXPECT_GE(max_am_hdr(), 64ul);
    EXPECT_LT(max_am_hdr(), min_am_bcopy);
}

UCS_TEST_P(test_ucp_am_nbx, zero_send)
{
    test_am_send_recv(0, max_am_hdr());
}

UCP_INSTANTIATE_TEST_CASE(test_ucp_am_nbx)


class test_ucp_am_nbx_dts : public test_ucp_am_nbx {
public:
    std::vector<ucp_test_param>
    static enum_test_params(const ucp_params_t& ctx_params,
                            const std::string& name,
                            const std::string& test_case_name,
                            const std::string& tls)
    {
        std::vector<ucp_test_param> result;
        int dt_bit_idx;

        ucs_for_each_bit(dt_bit_idx, UCS_MASK(DT_NUM)) {
            EXPECT_FALSE(UCS_BIT(dt_bit_idx) & REPLY_FLAG);
            generate_test_params_variant(ctx_params, name, test_case_name,
                                         tls, UCS_BIT(dt_bit_idx), result);
            generate_test_params_variant(ctx_params, name, test_case_name,
                                         tls, UCS_BIT(dt_bit_idx) | REPLY_FLAG,
                                         result);
        }

        return result;
    }

    void init()
    {
        test_ucp_am_nbx::init();

        m_dt = make_dt(GetParam().variant);
    }

    void cleanup()
    {
        destroy_dt(m_dt);

        test_ucp_am_nbx::cleanup();
    }

    unsigned get_send_flag()
    {
        return (GetParam().variant & REPLY_FLAG) ? UCP_AM_SEND_REPLY : 0;
    }
};

UCS_TEST_P(test_ucp_am_nbx_dts, short_send)
{
    test_am(1);
}

UCS_TEST_P(test_ucp_am_nbx_dts, short_bcopy_send, "ZCOPY_THRESH=-1",
                                                  "RNDV_THRESH=-1")
{
    test_am(4 * UCS_KBYTE);
}

UCS_TEST_P(test_ucp_am_nbx_dts, long_bcopy_send, "ZCOPY_THRESH=-1",
                                                 "RNDV_THRESH=-1")
{
    test_am(64 * UCS_KBYTE);
}

UCS_TEST_P(test_ucp_am_nbx_dts, short_zcopy_send, "ZCOPY_THRESH=1",
                                                  "RNDV_THRESH=-1")
{
    test_am(4 * UCS_KBYTE);
}

UCS_TEST_P(test_ucp_am_nbx_dts, long_zcopy_send, "ZCOPY_THRESH=1",
                                                 "RNDV_THRESH=-1")
{
    test_am(64 * UCS_KBYTE);
}

UCP_INSTANTIATE_TEST_CASE(test_ucp_am_nbx_dts)


class test_ucp_am_nbx_rndv: public test_ucp_am_nbx {
public:
    test_ucp_am_nbx_rndv()
    {
        m_rx_dt  = ucp_dt_make_contig(1);
        m_status = UCS_OK;
        modify_config("RNDV_THRESH", "128");
    }

    ucs_status_t am_data_handler(const void *header, size_t header_length,
                                 void *data, size_t length,
                                 const ucp_am_recv_param_t *rx_param)
    {
        EXPECT_FALSE(m_am_received);
        EXPECT_TRUE(rx_param->recv_attr & UCP_AM_RECV_ATTR_FLAG_RNDV);
        EXPECT_FALSE(rx_param->recv_attr & UCP_AM_RECV_ATTR_FLAG_DATA);

        m_rx_buf.resize(length, 'u');

        m_rx_dt_desc.make(m_rx_dt, &m_rx_buf[0], length);

        ucp_request_param_t params;
        params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK  |
                              UCP_OP_ATTR_FIELD_USER_DATA |
                              UCP_OP_ATTR_FIELD_DATATYPE  |
                              UCP_OP_ATTR_FLAG_NO_IMM_CMPL;
        params.datatype     = m_rx_dt_desc.dt();
        params.cb.recv_am   = am_data_recv_cb;
        params.user_data    = this;
        ucs_status_ptr_t sp = ucp_am_recv_data_nbx(receiver().worker(),
                                                   data, m_rx_dt_desc.buf(),
                                                   m_rx_dt_desc.count(),
                                                   &params);
        EXPECT_TRUE(UCS_PTR_IS_PTR(sp)) << "sp is: " << sp;
        ucp_request_release(sp);

        return UCS_INPROGRESS;
    }

    static void am_data_recv_cb(void *request, ucs_status_t status,
                                size_t length, void *user_data)
    {
        test_ucp_am_nbx_rndv *self = reinterpret_cast<test_ucp_am_nbx_rndv*>
                                                                    (user_data);
        ASSERT_FALSE(self->m_am_received);
        self->m_am_received = true;
        EXPECT_UCS_OK(status);
        EXPECT_EQ(self->m_rx_buf, std::vector<char>(length, 'd'));
    }

    static ucs_status_t am_data_reject_rndv_cb(void *arg, const void *header,
                                               size_t header_length, void *data,
                                               size_t length,
                                               const ucp_am_recv_param_t *param)
    {
        test_ucp_am_nbx_rndv *self = reinterpret_cast<test_ucp_am_nbx_rndv*>(arg);

        EXPECT_FALSE(self->m_am_received);
        self->m_am_received = true;

        return self->m_status;
    }

    static ucs_status_t am_data_deferred_reject_rndv_cb(void *arg,
                                                        const void *header,
                                                        size_t header_length,
                                                        void *data, size_t length,
                                                        const ucp_am_recv_param_t *param)
    {
        void **data_desc_p = reinterpret_cast<void**>(arg);

        EXPECT_EQ(NULL, *data_desc_p);
        *data_desc_p = data;

        return UCS_INPROGRESS;
    }

    ucp_datatype_t               m_rx_dt;
    ucs_status_t                 m_status;
    ucp::data_type_desc_t        m_rx_dt_desc;
    std::vector<char>            m_rx_buf;
};

UCS_TEST_P(test_ucp_am_nbx_rndv, rndv_auto, "RNDV_SCHEME=auto")
{
    test_am_send_recv(64 * UCS_KBYTE);
}

UCS_TEST_P(test_ucp_am_nbx_rndv, rndv_get, "RNDV_SCHEME=get_zcopy")
{
    test_am_send_recv(64 * UCS_KBYTE);
}

UCS_TEST_P(test_ucp_am_nbx_rndv, rndv_put, "RNDV_SCHEME=put_zcopy")
{
    test_am_send_recv(64 * UCS_KBYTE);
}

UCS_TEST_P(test_ucp_am_nbx_rndv, reject_rndv)
{
    skip_loopback();

    set_am_data_handler(receiver(), TEST_AM_NBX_ID, am_data_reject_rndv_cb,
                        this);

    std::vector<char> sbuf(10000, 0);
    ucp_request_param_t param;
    param.op_attr_mask      = 0ul;
    ucs_status_t statuses[] = {UCS_OK, UCS_ERR_REJECTED, UCS_ERR_NO_MEMORY};

    scoped_log_handler wrap_err(wrap_errors_logger);

    for (int i = 0; i < ucs_static_array_size(statuses); ++i) {
        m_am_received = false;
        m_status      = statuses[i];

        ucs_status_ptr_t sptr = ucp_am_send_nbx(sender().ep(), TEST_AM_NBX_ID,
                                                NULL, 0ul, sbuf.data(),
                                                sbuf.size(), &param);

        EXPECT_EQ(m_status, request_wait(sptr));
        EXPECT_TRUE(m_am_received);
    }
}

UCS_TEST_P(test_ucp_am_nbx_rndv, deferred_reject_rndv)
{
    skip_loopback();

    void *data_desc = NULL;
    std::vector<char> sbuf(10000, 0);
    ucp_request_param_t param;

    param.op_attr_mask = 0ul;

    set_am_data_handler(receiver(), TEST_AM_NBX_ID,
                        am_data_deferred_reject_rndv_cb, &data_desc);

    ucs_status_ptr_t sptr = ucp_am_send_nbx(sender().ep(), TEST_AM_NBX_ID,
                                            NULL, 0ul, sbuf.data(),
                                            sbuf.size(), &param);

    wait_for_flag(&data_desc);
    EXPECT_TRUE(data_desc != NULL);

    ucp_am_data_release(receiver().worker(), data_desc);
    EXPECT_EQ(UCS_OK, request_wait(sptr));
}

UCP_INSTANTIATE_TEST_CASE(test_ucp_am_nbx_rndv)


class test_ucp_am_nbx_rndv_dts: public test_ucp_am_nbx_rndv {
public:
    void init()
    {
        test_ucp_am_nbx::init();

        m_dt    = make_dt(dt_pairs()[GetParam().variant][0]);
        m_rx_dt = make_dt(dt_pairs()[GetParam().variant][1]);
    }

    void cleanup()
    {
        destroy_dt(m_dt);
        destroy_dt(m_rx_dt);

        test_ucp_am_nbx::cleanup();
    }

    std::vector<ucp_test_param>
    static enum_test_params(const ucp_params_t& ctx_params,
                            const std::string& name,
                            const std::string& test_case_name,
                            const std::string& tls)
    {
        std::vector<ucp_test_param> result;

        for (int i = 0; i < dt_pairs().size(); ++i) {
            generate_test_params_variant(ctx_params, name, test_case_name,
                                         tls, i, result);
        }

        return result;
    }

    static std::vector<std::vector<int> > &dt_pairs()
    {
        static std::vector<std::vector<int> > res;

        if (res.empty()) {
            int dt_arr[] = {DT_CONTIG, DT_IOV, DT_GENERIC};
            std::vector<int> dts(dt_arr, dt_arr + ucs_static_array_size(dt_arr));

            res = ucs::make_pairs(dts);
        }

        return res;
    }
};

UCS_TEST_P(test_ucp_am_nbx_rndv_dts, rndv, "RNDV_THRESH=256")
{
    test_am_send_recv(64 * UCS_KBYTE);
}

UCP_INSTANTIATE_TEST_CASE(test_ucp_am_nbx_rndv_dts);
