#ifndef BASEDEF_H_
#define BASEDEF_H_

#define	A_OUT						fprintf(stderr, "[%s][%s][%d]", __FILE__, __FUNCTION__, __LINE__);
#define	A_FPRINTF(fmt, arg...)		A_OUT fprintf(stderr, fmt, ##arg)

//#define CCTT_I
#define CCTT_II

//国网台体测试
#define GW_TAI_TI

#endif//BASEDEF_H_
