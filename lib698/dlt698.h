

#ifndef DLT698_H_
#define DLT698_H_
#include "Objectdef.h"

extern int doObjectAction(OAD oad,INT8U *data);
extern int StateProcess(int* step,int* rev_delay,int delay_num, int* rev_tail,int* rev_head,unsigned char *NetRevBuf,unsigned char* dealbuf);
extern int ProcessData(CommBlock *com);
extern int Link_Request(LINK_Request request,INT8U *addr,INT8U *buf);

/*----------------------抄表相关*************************/
INT16S composeProtocol698_GetRequest(INT8U*,CLASS_6015,TSA);
#endif
