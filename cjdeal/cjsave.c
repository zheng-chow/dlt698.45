

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <pthread.h>
#include "sys/reboot.h"
#include <wait.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "cjsave.h"
/*
 * 计算总共有几个oad，来确定给文件头流出多少空间
 * Task文件头格式：
 * 2个字节：文件头长度
 * 2个字节：一个TSA长度
 * 文件头：n个HEAD_UNIT结构
 *       1.  202a-0200:服务器地址
 *       2.  6040-0200:采集启动时标
 *       3.  6041-0200：采集成功时标
 *       4.  6042-0200：采集存储时标
 *       5...n  任务CSD描述
 */
INT16U FixHeadUnit(INT8U *headbuf,INT8U *fixlen)
{
	static INT8U head_oad[4][4]={{0x20,0x2a,0x02,0x00},{0x60,0x40,0x02,0x00},{0x60,0x41,0x02,0x00},{0x60,0x42,0x02,0x00}};
	static INT8U head_oad_len[4]={0x0012,0x0008,0x0008,0x0008};
	int	  i=0,index=0;
	HEAD_UNIT	unit[4]={};
	*fixlen = 0;
	for(i=0;i<4;i++) {
		memset(&unit[i].oad_m,0,sizeof(OAD));
		unit[i].oad_r.OI = head_oad[i][0];
		unit[i].oad_r.OI = (unit[i].oad_r.OI<<8) | head_oad[i][1];
		unit[i].oad_r.attflg = head_oad[i][2];
		unit[i].len = head_oad_len[i];
		*fixlen += head_oad_len[i];
	}
	if(headbuf!=NULL) {
		memcpy(headbuf,unit,sizeof(unit));
		index += sizeof(unit);
	}
	return index;
}

INT16U CalcHeadRcsdUnitNum(CSD_ARRAYTYPE csds)
{
	INT16U headunit_num=4;		//FixHeadUnit 固定TSA+3个时标的HEAD_UNIT长度
	int i=0;
	if(csds.num>MY_CSD_NUM) {
		fprintf(stderr,"rcsd 个数超过限值 %d!!!!!!!!!!\n",MY_CSD_NUM);
		csds.num = MY_CSD_NUM;
	}
	for(i=0;i<csds.num;i++)
	{
		if(csds.csd[i].type != 0 && csds.csd[i].type != 1)
			continue;
		if(csds.csd[i].type == 1)
			headunit_num += csds.csd[i].csd.road.num+1;//加上本身
		else
			headunit_num++;
	}
	if(headunit_num == 4)
		return 0;
	else
		return headunit_num;
}
/*
 * 计算某个OI的数据长度，指针对抄表数据 todo 先写个简单的，以后完善 而且没有考虑费率
 * attr_flg:0 全部属性 非0 一个属性  例如20000200 则为全部属性 20000201则为一个属性
 * OI_TYPE.cfg格式定义：
 * 对象标识OI-数据类型描述-数据长度-接口类IC-单位换算
 * 2000-12-002-03-11 解析：
 * 2000：OI 电压
 * 12：long-unsigned
 * 002:1个数据长度
 * 03：接口类IC 变量类参数
 * 11：换算：-1   如果12：表示换算：-2,  02：表示换算：+2
 */
INT16U CalcOILen(OAD oad,INT8U rate)
{
	FILE *fp;
	char ln[60];
	char lnf[4];
	INT16U oi_len=0;
	INT8U ic_type = 1;

	//TODO:  MET_RATE 替换成6000档案的电表费率个数rate
	if(oad.OI>=0x0000 && oad.OI<0x2000)		//接口IC的1,2类，每个数据长度固定为4个字节
	{
		if(oad.attrindex == 0){	//全部属性
			oi_len += 2;			//数组+元素个数
			oi_len += 5*(MET_RATE+1);	//5:数据类型描述+数据,(MET_RATE+1):总及4费率
			return oi_len;
		}
		else
			return (4+1);	//4:数据长度+1个字节数据类型
	}
	fp = fopen("/nor/config/OI_TYPE.cfg","r");
	if(fp == NULL)
	{
		fprintf(stderr,"\nOI_TYPE.cfg do not exist,hard error!!\n");
		return 0;
	}
	while(1)
	{
		memset(ln,0x00,60);
		fscanf(fp,"%s",ln);
		if(strncmp(ln,"begin",5) == 0) continue;
		if(strncmp(ln,"end",3) == 0) break;
		if(strncmp(ln,"//",2) == 0) continue;

		memset(lnf,0x00,4);
		memcpy(lnf,&ln[0],4);

		if(strtol(lnf,NULL,16) != oad.OI)
			continue;
		memset(lnf,0x00,4);
		memcpy(lnf,&ln[8],3);
		oi_len = strtol(lnf,NULL,10)+1;
		memset(lnf,0x00,4);
		memcpy(lnf,&ln[12],2);
		ic_type = strtol(lnf,NULL,10);
		break;
	}
	fclose(fp);
	if(oi_len != 0 && ic_type != 0)
	{
		switch(ic_type)
		{
		case 3:	//分相变量接口类
			if(oad.attrindex == 0)
				oi_len = oi_len*3+1+1;//三相
			break;
		case 4://功率接口类
			if(oad.attrindex == 0)
				oi_len = oi_len*4+1+1;//总及分项
			break;
		default:
			break;
		}
	}
	return oi_len;	//返回长度+1个字节数据类型描述
}

INT8U getOneUnit(INT8U *headbuf,OAD oad_m,OAD oad_r,INT16U len)
{
	HEAD_UNIT	headunit={};

	memcpy(&headunit.oad_m,&oad_m,sizeof(OAD));
	memcpy(&headunit.oad_r,&oad_r,sizeof(OAD));
	headunit.len = len;
	memcpy(headbuf,&headunit,sizeof(HEAD_UNIT));
	return (sizeof(HEAD_UNIT));
}

/*
 * 结构为四个字节长度+TSA(0x00+40010200+2个字节长度)+3*时标+CSD
 * unitlen_z长度为此任务当日需要抄的全部数据长度，以此将一个测量点一天的数据放在一个地方
 */
void CreateSaveHead(char *fname,CSD_ARRAYTYPE csds,INT16U *headlen,INT16U *unitlen,INT16U *unitnum,INT16U freq,INT8U wrflg)
{
	INT16U pindex=0,len_tmp=0,csd_unitnum=0;
	int i=0,j=0;
	INT8U *headbuf=NULL,fixlen=0;
	OAD	 oad_m={};
	*unitlen = 0;


	if(csds.num == 0xee || csds.num == 0)
		return;
	csd_unitnum = CalcHeadRcsdUnitNum(csds);
	fprintf(stderr,"\n---csd_unitnum = %d\n",csd_unitnum);
	if(headbuf == NULL) {
		*headlen=csd_unitnum*sizeof(HEAD_UNIT)+4;	//4:文件头长度+TSA块长度
		headbuf = (INT8U *)malloc(*headlen);
	}
	headbuf[pindex++] = (*headlen & 0xff00)>>8;//文件头长度
	headbuf[pindex++] = (*headlen & 0x00ff);
	headbuf[pindex++] = 0x00;
	headbuf[pindex++] = 0x00;//长度
	pindex += FixHeadUnit(&headbuf[pindex],&fixlen);
	if(csds.num > MY_CSD_NUM)//超了
		csds.num = MY_CSD_NUM;
	for(i=0;i<csds.num;i++)
	{
		if(csds.csd[i].type == 0xee)
			break;
		if(csds.csd[i].type != 0 && csds.csd[i].type != 1)
			continue;
		if(csds.csd[i].type == 0)	//OAD
		{
			fprintf(stderr,"\n-0--csds.csd[i].csd.oad.OI = %04x\n",csds.csd[i].csd.oad.OI);
			len_tmp = CalcOILen(csds.csd[i].csd.oad,4);//多一个数据类型
			fprintf(stderr,"\nlen_tmp=%d\n",len_tmp);
			memset(&oad_m,0,sizeof(OAD));
			pindex += getOneUnit(&headbuf[pindex],oad_m,csds.csd[i].csd.oad,len_tmp);
			*unitlen += len_tmp;
			(*unitnum)++;
			fprintf(stderr,"\n-1-unitlen=%d\n",*unitlen);
		}else if(csds.csd[i].type == 1)	//ROAD
		{
			if(csds.csd[i].csd.road.num == 0xee)
				continue;
			if(csds.csd[i].csd.road.num > ROAD_OADS_NUM)//超了
				csds.csd[i].csd.road.num = ROAD_OADS_NUM;
			for(j=0;j<csds.csd[i].csd.road.num;j++)
			{
				fprintf(stderr,"\n-2--csds.csd[i].csd.oad.OI = %04x\n",csds.csd[i].csd.road.oads[j].OI);
				if(csds.csd[i].csd.road.oads[j].OI == 0xeeee)
					break;
				len_tmp = CalcOILen(csds.csd[i].csd.road.oads[j],4);//多一个数据类型
				fprintf(stderr,"\n--2-len_tmp=%d\n",len_tmp);
				pindex += getOneUnit(&headbuf[pindex],csds.csd[i].csd.road.oad,csds.csd[i].csd.road.oads[j],len_tmp);
				*unitlen += len_tmp;
				(*unitnum)++;
			}
		}
	}
	fprintf(stderr,"\n-2-unitlen=%d\n",*unitlen);
	*unitlen=freq*(fixlen+*unitlen);//一个单元存储TSA共用,在结构最前面，每个单元都有3个时标和数据，预留出合适大小，以能存下一个TSA所有数据
	fprintf(stderr,"\n-3-unitlen=%d\n",*unitlen);
	headbuf[2] = (*unitlen & 0xff00) >> 8;//数据单元长度
	headbuf[3] = *unitlen & 0x00ff;
	fprintf(stderr,"\nhead(%d)::",pindex);
	for(i=0;i<pindex;i++)
	{
		fprintf(stderr," %02x",headbuf[i]);
	}
	fprintf(stderr,"\n");
	if(wrflg==1)
	{
		datafile_write(fname, headbuf, *headlen, 0);
	}
	if(headbuf) {
		free(headbuf);
	}
}
/*
 * 读取文件头长度和块数据长度
 */
void ReadFileHeadLen(FILE *fp,INT16U *headlen,INT16U *blocklen)
{
	INT16U headlength=0,blocklength=0;
	fread(&headlength,2,1,fp);
	*headlen = ((headlength>>8)+((headlength&0xff)<<8));
	fread(&blocklength,2,1,fp);
	*blocklen = ((blocklength>>8)+((blocklength&0xff)<<8));
}
void ReadFileHead(char *fname,INT16U headlen,INT16U unitlen,INT16U unitnum,INT8U *headbuf)
{
	FILE *fp = NULL;
	HEAD_UNIT *headunit = NULL;
//	INT16U unitlen = 0,unitnum = 0;
//	INT16U headlen=0;
	int i=0;
	fp = fopen(fname,"r");
	if(fp==NULL)
	{
		fprintf(stderr,"\n%s open error,file not exist!\n",fname);
		return ;
	}
	else
	{
		fread(headbuf,headlen,1,fp);
		headunit = malloc(headlen-4);
		memcpy(headunit,&headbuf[4],headlen-4);
		for(i=0;i<unitnum;i++)
		{
			fprintf(stderr,"oad_m:%04x%02x%02x:oad_r:%04x%02x%02x:len=%d\n",
					headunit[i].oad_m.OI,headunit[i].oad_m.attflg,headunit[i].oad_m.attrindex,
					headunit[i].oad_r.OI,headunit[i].oad_r.attflg,headunit[i].oad_r.attrindex,headunit[i].len);
		}
		fprintf(stderr,"\n");
		free(headunit);
		fclose(fp);
	}
}


/*
 * 存储普通采集方案数据，
 * 数据格式：文件头结构：标注文件TSA和时标及csd格式，开始4个字节为文件头长度和每个数据单元长度 数据结构：存储各数据项值
 * 分测量点存储，一个TSA的全部数据放到一起，例如24个点的曲线数据，则按采集个数编号放入到一个位置,减少索引时间
 */
void SaveNorData(INT8U taskid,INT8U *databuf,int datalen)
{
	TS ts_now;
	FILE *fp;
	char	fname[FILENAMELEN]={};
	INT8U *databuf_tmp=NULL;
	int savepos=0,currpos=0,i=0;
	INT16U headlen=0,unitlen=0,unitnum=0,unitseq=0;
//	CLASS_6015	class6015={};
//	CLASS_6013	class6013={};
	TASKSET_INFO tasknor_info;
	memset(fname,0,sizeof(fname));
	TSGet(&ts_now);//用的当前时间，测试用，需要根据具体存储时标选择来定义
	if(ReadTaskInfo(taskid,&tasknor_info)!=1)
		return;
	getTaskFileName(taskid,ts_now,fname);
	fp = fopen(fname,"r");
	if(fp == NULL)//文件没内容 组文件头，如果文件已存在，提取文件头信息
	{
		CreateSaveHead(fname,tasknor_info.csds,&headlen,&unitlen,&unitnum,tasknor_info.runtime,1);//写文件头信息并返回
		databuf_tmp = malloc(unitlen);
		savepos=0;
	}
	else
	{
		ReadFileHeadLen(fp,&headlen,&unitlen);
		databuf_tmp = malloc(unitlen);
		fseek(fp,headlen,SEEK_SET);//跳过文件头
		while(!feof(fp))
		{
			if(fread(databuf_tmp,unitlen,1,fp)==0)
			{
//				fseek(fp,headlen,SEEK_SET);//回到文件头末尾
				fprintf(stderr,"\n-----savepos=%d,ftell(fp)=%d\n",savepos,(int)ftell(fp));
				break;
			}
			fprintf(stderr,"\n文件中地址：");
			for(i=0;i<17;i++)
			{
				fprintf(stderr," %02x",databuf_tmp[i]);
			}
			fprintf(stderr,"\n数据中地址");
			for(i=0;i<17;i++)
			{
				fprintf(stderr," %02x",databuf[i]);
			}
			fprintf(stderr,"\n");
			if(memcmp(databuf_tmp,databuf,17)==0)//找到了存储结构的位置，一个存储结构可能含有unitnum个单元
			{
				savepos=ftell(fp)-unitlen;
				break;
			}
			else
				fprintf(stderr,"\n-----savepos=%d,ftell(fp)=%d\n",savepos,(int)ftell(fp));
		}
	}
	if(fp != NULL)
		currpos = ftell(fp);
	fprintf(stderr,"\n-------%%%%@@@------currpos=%d:%d\n",currpos,headlen+unitlen);
	if(savepos==0)//存储位置为0.说明文件中没找到，则应添加而不是覆盖
	{
		if(currpos == 0)//第一个存储的
			savepos=headlen;
		else
			savepos=currpos;
		fprintf(stderr,"\n-------1------savepos=%d\n",savepos);
		memset(databuf_tmp,0x00,unitlen);
		for(i=0;i<tasknor_info.runtime;i++)
			memcpy(&databuf_tmp[unitlen*i/tasknor_info.runtime],databuf,17);//每个小单元地址附上
	}
	unitseq = (ts_now.Hour*60*60+ts_now.Minute*60+ts_now.Sec)/((24*60*60)/tasknor_info.runtime)+1;
	fprintf(stderr,"\n-----unitseq=%d\n",unitseq);
	if(unitseq > tasknor_info.runtime)
	{
		if(databuf_tmp != NULL)
			free(databuf_tmp);
		return ;//出错了，序列号超过了总长度
	}
	fprintf(stderr,"\n-----unitlen*(unitseq-1)/tasknor_info.runtime=%d\n",unitlen*(unitseq-1)/tasknor_info.runtime);
	memcpy(&databuf_tmp[unitlen*(unitseq-1)/tasknor_info.runtime],databuf,datalen);
	if(datalen != unitlen/tasknor_info.runtime)
	{
		fprintf(stderr,"\n----长度不对，不保存datalen=%d unitlen=%d,tasknor_info.runtime=%d\n",datalen,unitlen,tasknor_info.runtime);
		if(databuf_tmp != NULL)
			free(databuf_tmp);
		return ;//长度不对
	}
	else
	{
		fprintf(stderr,"\n----存储savepos=%d,unitlen=%d\n",savepos,unitlen);
		datafile_write(fname, databuf_tmp, unitlen, savepos);
	}
	if(fp!=NULL)
		fclose(fp);
	if(databuf_tmp != NULL)
		free(databuf_tmp);
}
///*
// * 读取抄表数据，读取某个测量点某任务某天一整块数据，放在内存里，根据需要提取数据,并根据csd
// */
//void ReadNorData(TS ts,INT8U taskid,INT8U *tsa)
//{
//	FILE *fp  = NULL;
//	INT16U headlen=0,unitlen=0,unitnum=0,readlen=0;
//	INT8U *databuf_tmp=NULL;
//	INT8U *headbuf=NULL;
//	int i=0,j=0;
//	int savepos=0;
//	TS ts_now;
//	TSGet(&ts_now);
//	char	fname[FILENAMELEN]={};
//	TASKSET_INFO tasknor_info;
//	taskid=1;
//	if(ReadTaskInfo(taskid,&tasknor_info)!=1)
//		return;
//	getTaskFileName(taskid,ts_now,fname);
//	ReadFileHeadLen(fname,&headlen,&unitlen);
//	headbuf = (INT8U *)malloc(headlen);
//	unitnum = (headlen-4)/sizeof(HEAD_UNIT);
//	ReadFileHead(fname,headlen,unitlen,unitnum,headbuf);
//	databuf_tmp = malloc(unitlen);
//	fp = fopen(fname,"r");
//	if(fp == NULL)//文件没内容 组文件头，如果文件已存在，提取文件头信息
//	{
//		return;
//	}
//	else
//	{
//		fseek(fp,headlen,SEEK_SET);//跳过文件头
//		while(!feof(fp))//找存储结构位置
//		{
//			//00 00 00 00 00 00 00 00 07 05 00 00 00 00 00 01
//			//00 00 00 00 00 00 00 00 00 07 05 00 00 00 00 00 01
//			readlen = fread(databuf_tmp,unitlen,1,fp);
//			fprintf(stderr,"\n----读出来的长度readlen=%d,块大小unitlen=%d\n",readlen,unitlen);
//			if(readlen == 0)
//				break;
//			fprintf(stderr,"\n");
//			fprintf(stderr,"\n传进来的TSA：");
//			for(i=0;i<TSA_LEN;i++)
//				fprintf(stderr," %02x",tsa[i]);
//			for(i=0;i<TSA_LEN;i++)
//				fprintf(stderr," %02x",databuf_tmp[i+1]);
//			if(memcmp(&databuf_tmp[1],&tsa[0],17)==0)//找到了存储结构的位置，一个存储结构可能含有unitnum个单元
//				savepos=ftell(fp)-unitlen;//对应的放到对应的位置
//		}
//		fprintf(stderr,"\n----(pos:%d):\n",savepos);
//		if(savepos==0)//没找到的不打印
//			return;
//		fprintf(stderr,"\n要求的tsa:");
//		for(j=0;j<17;j++)
//			fprintf(stderr,"%02x ",tsa[j]);
//		fprintf(stderr,"\n文件中的数据(pos:%d):",savepos);
//		fseek(fp,savepos,SEEK_SET);//
//		fread(databuf_tmp,unitlen,1,fp);
////		for(j=0;j<unitlen;j++)
////		{
////			fprintf(stderr,"%02x ",databuf_tmp[j]);
////		}
//		fprintf(stderr,"\n文件%s存储的数据(%d)：",fname,unitlen);
//		fprintf(stderr,"\n");
//
//		for(i=0;i<unitlen;i++)
//		{
//			if(i%(unitlen/tasknor_info.runtime) == 0)
//				fprintf(stderr,"\n%d:",i/(unitlen/tasknor_info.runtime));
//
//			fprintf(stderr," %02x",databuf_tmp[i]);
//		}
//
//	}
//}
//INT16U GetFileOadLen(INT8U units,INT8U tens)//个位十位转化为一个INT16U
//{
//	INT16U total = 0;
//	total = tens;
//	return (total<<8)+units;
//}
/*
 * 读取抄表数据，读取某个测量点某任务某天一整块数据，放在内存里，根据需要提取数据,并根据csd组报文
 */
//int ComposeSendBuff(TS *ts,INT8U seletype,INT8U taskid,TSA *tsa_con,INT8U tsa_num,CSD_ARRAYTYPE csds,INT8U *SendBuf)
//{
//	OAD  oadm,oadr;
//	FILE *fp  = NULL;
//	INT16U headlen=0,blocklen=0,unitnum=0,freq=0,sendindex=0,retlen=0,schpos=0;
//	INT8U *databuf_tmp=NULL;
//	INT8U tmpbuf[256];
//	INT8U headl[2],blockl[2];
//	int i=0,j=0,k=0,m=0;
//	CLASS_6015	class6015={};
//	CLASS_6013	class6013={};
//	HEAD_UNIT *head_unit = NULL;
//	TS ts_now;
//	TSGet(&ts_now);
//	char	fname[FILENAMELEN]={};
//	memset(&class6013,0,sizeof(CLASS_6013));
//	readCoverClass(0x6013,taskid,&class6013,sizeof(class6013),coll_para_save);
//	memset(&class6015,0,sizeof(CLASS_6015));
//	readCoverClass(0x6015,class6013.sernum,&class6015,sizeof(CLASS_6015),coll_para_save);
//	freq = CalcFreq(class6015);
//	////////////////////////////////////////////////////////////////////////////////test
//	memset(&class6015,0xee,sizeof(CLASS_6015));
//	class6015.csds.num = 1;
//	class6015.csds.csd[0].type=1;
//	class6015.csds.csd[0].csd.road.oad.OI =0x5004;
//	class6015.csds.csd[0].csd.road.oad.attflg = 0x02;
//	class6015.csds.csd[0].csd.road.oad.attrindex = 0x00;
//	class6015.csds.csd[0].csd.road.num = 3;
//	class6015.csds.csd[0].csd.road.oads[0].OI = 0x2021;
//	class6015.csds.csd[0].csd.road.oads[0].attflg = 0x02;
//	class6015.csds.csd[0].csd.road.oads[0].attrindex = 0x00;
//	class6015.csds.csd[0].csd.road.oads[1].OI = 0x0010;
//	class6015.csds.csd[0].csd.road.oads[1].attflg = 0x02;
//	class6015.csds.csd[0].csd.road.oads[1].attrindex = 0x00;
//	class6015.csds.csd[0].csd.road.oads[2].OI = 0x0020;
//	class6015.csds.csd[0].csd.road.oads[2].attflg = 0x02;
//	class6015.csds.csd[0].csd.road.oads[2].attrindex = 0x00;
//	freq = 1;
//	taskid=1;
//	//////////////////////////////////////////////////////////////////////////////////test
//
//	getTaskFileName(taskid,ts_now,fname);
////	ReadFileHeadLen(fname,&headlen,&unitlen);
////	headbuf = (INT8U *)malloc(headlen);
////	unitnum = (headlen-4)/sizeof(HEAD_UNIT);
////	ReadFileHead(fname,headlen,unitlen,unitnum,headbuf);
////	databuf_tmp = malloc(unitlen);
////	savepos = (int *)malloc(tsa_num*sizeof(int));
////	fprintf(stderr,"\n-------------6--%s\n",fname);
//	fp = fopen(fname,"r");
//	if(fp == NULL)//文件没内容 组文件头，如果文件已存在，提取文件头信息
//	{
//		return 0;
//	}
//	else
//	{
////		fprintf(stderr,"\n-------------7\n");
//		fread(headl,2,1,fp);
//		headlen = headl[0];
//		headlen = (headl[0]<<8) + headl[1];
//		fread(&blockl,2,1,fp);
////		fprintf(stderr,"\nblocklen=%02x %02x\n",blockl[1],blockl[0]);
//		blocklen = blockl[0];
//		blocklen = (blockl[0]<<8) + blockl[1];
//		unitnum = (headlen-4)/sizeof(HEAD_UNIT);
//		databuf_tmp = (INT8U *)malloc(blocklen);
//
//		head_unit = (HEAD_UNIT *)malloc(headlen-4);
//		fread(head_unit,headlen-4,1,fp);
//
//		fseek(fp,headlen,SEEK_SET);//跳过文件头
//		while(!feof(fp))//找存储结构位置
//		{
////			fprintf(stderr,"\n-------------8---blocklen=%d,headlen=%d,tsa_num=%d\n",blocklen,headlen,tsa_num);
//			//00 00 00 00 00 00 00 00 07 05 00 00 00 00 00 01
//			//00 00 00 00 00 00 00 00 00 07 05 00 00 00 00 00 01
//			memset(databuf_tmp,0xee,blocklen);
//			if(fread(databuf_tmp,blocklen,1,fp) == 0)
//				break;
//			for(i=0;i<tsa_num;i++)
//			{
//				for(m=0;m<freq;m++)
//				{
//					schpos = m*blocklen/freq;
////					fprintf(stderr,"\n-------------9---i=%d\n",i);
////					fprintf(stderr,"\n1addr1:%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
////							databuf_tmp[schpos+16],databuf_tmp[schpos+15],databuf_tmp[schpos+14],databuf_tmp[schpos+13],
////							databuf_tmp[schpos+12],databuf_tmp[schpos+11],databuf_tmp[schpos+10],databuf_tmp[schpos+9],
////							databuf_tmp[schpos+8],databuf_tmp[schpos+7],databuf_tmp[schpos+6],	databuf_tmp[schpos+5],
////							databuf_tmp[schpos+4],databuf_tmp[schpos+3],databuf_tmp[schpos+2],databuf_tmp[schpos+1]);
////					fprintf(stderr,"\n1addr2:%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
////							tsa_con[i].addr[16],tsa_con[i].addr[15],tsa_con[i].addr[14],tsa_con[i].addr[13],
////							tsa_con[i].addr[12],tsa_con[i].addr[11],tsa_con[i].addr[10],tsa_con[i].addr[9],
////							tsa_con[i].addr[8],tsa_con[i].addr[7],tsa_con[i].addr[6],	tsa_con[i].addr[5],
////							tsa_con[i].addr[4],tsa_con[i].addr[3],tsa_con[i].addr[2],tsa_con[i].addr[1],tsa_con[i].addr[0]);
//					if(memcmp(&databuf_tmp[schpos+1],&tsa_con[i].addr[1],16)==0)//找到了存储结构的位置，一个存储结构可能含有unitnum个单元
//					{
//						fprintf(stderr,"\naddr1:%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
//								databuf_tmp[schpos+16],databuf_tmp[schpos+15],databuf_tmp[schpos+14],databuf_tmp[schpos+13],
//								databuf_tmp[schpos+12],databuf_tmp[schpos+11],databuf_tmp[schpos+10],databuf_tmp[schpos+9],
//								databuf_tmp[schpos+8],databuf_tmp[schpos+7],databuf_tmp[schpos+6],	databuf_tmp[schpos+5],
//								databuf_tmp[schpos+4],databuf_tmp[schpos+3],databuf_tmp[schpos+2],databuf_tmp[schpos+1]);
//						fprintf(stderr,"\naddr2:%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
//								databuf_tmp[schpos+16],databuf_tmp[schpos+15],databuf_tmp[schpos+14],databuf_tmp[schpos+13],
//								databuf_tmp[schpos+12],databuf_tmp[schpos+11],databuf_tmp[schpos+10],databuf_tmp[schpos+9],
//								databuf_tmp[schpos+8],databuf_tmp[schpos+7],databuf_tmp[schpos+6],	databuf_tmp[schpos+5],
//								databuf_tmp[schpos+4],databuf_tmp[schpos+3],databuf_tmp[schpos+2],databuf_tmp[schpos+1]);
//						for(j=0;j<csds.num;j++)
//						{
////							fprintf(stderr,"\n-------%d:(type=%d)\n",j,csds.csd[j].type);
//							if(csds.csd[j].type != 0 && csds.csd[j].type != 1)
//								continue;
//							if(csds.csd[j].type == 1)
//							{
//								SendBuf[sendindex++] = 0x01;//aray
//								SendBuf[sendindex++] = csds.csd[j].csd.road.num;
//								for(k=0;k<csds.csd[j].csd.road.num;k++)
//								{
//									memset(tmpbuf,0x00,256);
//									memcpy(&oadm,&csds.csd[j].csd.road.oads[k],sizeof(OAD));
//									memcpy(&oadr,&csds.csd[j].csd.road.oad,sizeof(OAD));
//									fprintf(stderr,"\nmaster oad:%04x%02x%02x\n",oadm.OI,oadm.attflg,oadm.attrindex);
//									fprintf(stderr,"\nrelate oad:%04x%02x%02x\n",oadr.OI,oadr.attflg,oadr.attrindex);
//									if((retlen = GetPosofOAD(&databuf_tmp[schpos],oadm,oadr,head_unit,unitnum,tmpbuf))==0)
//										SendBuf[sendindex++] = 0;
//									else
//									{
//										fprintf(stderr,"\n--type=1--tmpbuf[0]=%02x\n",tmpbuf[0]);
//										if(tmpbuf[0]==0)//NULL
//											SendBuf[sendindex++] = 0;
//										else
//										{
//											memcpy(&SendBuf[sendindex],tmpbuf,retlen);
//											sendindex += retlen;
//										}
//									}
////									fprintf(stderr,"\n---1----k=%d retlen=%d\n",k,retlen);
//								}
//							}
//							if(csds.csd[j].type == 0)
//							{
//								if(csds.csd[j].csd.oad.OI == 0x4001)
//								{
//									SendBuf[sendindex++]=0x55;
//									for(k=1;k<17;k++)
//									{
////										fprintf(stderr,"\n--databuf_tmp[%d+%d]=%d\n",schpos,k,databuf_tmp[schpos+k]);
//										if(databuf_tmp[schpos+k]==0)
//											continue;
//										memcpy(&SendBuf[sendindex],&databuf_tmp[schpos+k],databuf_tmp[schpos+k]+1);
//										sendindex += databuf_tmp[schpos+k]+1;
//										break;
//									}
//									continue;
//								}
//								memset(tmpbuf,0x00,256);
//								memcpy(&oadm,&csds.csd[j].csd.oad,sizeof(OAD));
//								memset(&oadr,0xee,sizeof(OAD));
//								if((retlen = GetPosofOAD(&databuf_tmp[schpos],oadm,oadr,head_unit,unitnum,tmpbuf))==0)
//									SendBuf[sendindex++] = 0;
//								else
//								{
////									fprintf(stderr,"\n--type=0--tmpbuf[0]=%02x\n",tmpbuf[0]);
//									if(tmpbuf[0]==0)//NULL
//										SendBuf[sendindex++] = 0;
//									else
//									{
//										memcpy(&SendBuf[sendindex],tmpbuf,retlen);
//										sendindex += retlen;
//									}
//								}
////								fprintf(stderr,"\n---0----k=%d retlen=%d\n",k,retlen);
//							}
//						}
//	//					return sendindex;
//						continue;
//					}
//				}
//			}
//		}
//	}
//	return sendindex;
//}
//int main(int argc, char *argv[])
//{
////    struct sigaction sa = {};
////    Setsig(&sa, QuitProcess);
//	RSD select;
//	INT8U selectype;
//	CSD_ARRAYTYPE csds;
//	INT8U data[1024];
//	int datalen,i=0;
//
//	INT8U data_tmp1[] = {
//			0x55,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x05,0x00,0x00,0x00,0x00,0x00,0x01,//TSA，head 17
//			0x1c,0x07,0xe0,0x09,0x0d,0x00,0x00,0x00,//时标 head                                              8
//			0x1c,0x07,0xe0,0x09,0x0d,0x00,0x00,0x0f,//时标 head                                              8
//			0x1c,0x07,0xe0,0x09,0x0c,0x00,0x00,0x00,//时标 head                                              8
//			0x1c,0x07,0xe0,0x09,0x0c,0x00,0x00,0x00,//冻结时标 data 2021                                      8
//			0x01,0x05,0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00,          //27
//			0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00,//data 0010
//			0x01,0x05,0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00,          //27
//			0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00 //data 0020
//	};
//	INT8U data_tmp2[] = {
//			0x55,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x05,0x00,0x00,0x00,0x00,0x00,0x02,//TSA，head
//			0x1c,0x07,0xe0,0x09,0x0d,0x00,0x00,0x00,//时标 head
//			0x1c,0x07,0xe0,0x09,0x0d,0x00,0x00,0x0f,//时标 head
//			0x1c,0x07,0xe0,0x09,0x0c,0x00,0x00,0x00,//时标 head
//			0x1c,0x07,0xe0,0x09,0x0c,0x00,0x00,0x00,//冻结时标 data 2021
//			0x01,0x05,0x06,0x00,0x00,0x00,0x01,0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00,
//			0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x01,//data 0010
//			0x01,0x05,0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00,
//			0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00 //data 0020
//	};
//	INT8U data_tmp3[] = {
//			0x55,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x05,0x00,0x00,0x00,0x00,0x00,0x03,//TSA，head
//			0x1c,0x07,0xe0,0x09,0x0d,0x00,0x00,0x00,//时标 head
//			0x1c,0x07,0xe0,0x09,0x0d,0x00,0x00,0x0f,//时标 head
//			0x1c,0x07,0xe0,0x09,0x0c,0x00,0x00,0x00,//时标 head
//			0x1c,0x07,0xe0,0x09,0x0c,0x00,0x00,0x00,//冻结时标 data 2021
//			0x01,0x05,0x06,0x00,0x00,0x00,0x02,0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00,
//			0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x02,//data 0010
//			0x01,0x05,0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00,
//			0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00 //data 0020
//	};
//	INT8U data_tmp4[] = {
//			0x55,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x05,0x00,0x00,0x00,0x00,0x00,0x01,//TSA，head
//			0x1c,0x07,0xe0,0x09,0x0d,0x00,0x00,0x00,//时标 head
//			0x1c,0x07,0xe0,0x09,0x0d,0x00,0x00,0x0f,//时标 head
//			0x1c,0x07,0xe0,0x09,0x0c,0x00,0x00,0x00,//时标 head
//			0x1c,0x07,0xe0,0x09,0x0c,0x00,0x00,0x00,//冻结时标 data 2021
//			0x01,0x05,0x06,0x00,0x00,0x00,0x02,0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00,
//			0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x02,//data 0010
//			0x01,0x05,0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00,
//			0x06,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00 //data 0020
//	};
//	INT8U addr_tmp[] = {
//			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x05,0x00,0x00,0x00,0x00,0x00,0x01,
//			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x05,0x00,0x00,0x00,0x00,0x00,0x02,
//			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x05,0x00,0x00,0x00,0x00,0x00,0x03
//	};
//	TSA tsa[3];
//	memcpy(tsa[0].addr,&addr_tmp[0],17*3);
//
//
//	TS ts;
//	TSGet(&ts);
//	SaveNorData(1,data_tmp1,sizeof(data_tmp1));
//	fprintf(stderr,"\n--12--\n");
//	SaveNorData(1,data_tmp2,sizeof(data_tmp2));
//	SaveNorData(1,data_tmp3,sizeof(data_tmp3));
//	selectype = 5;
//	fprintf(stderr,"\n---getSelector入口\n");
//	getSelector(select,selectype, csds, data, &datalen);
//	fprintf(stderr,"\n报文(%d)：",datalen);
//	for(i=0;i<datalen;i++)
//		fprintf(stderr," %02x",data[i]);
//	fprintf(stderr,"\n");
//
////	SaveNorData(2,data_tmp4,sizeof(data_tmp4));//负荷曲线
//
//
//
//	OI_698 OI_tmp[9] =
//	{
//			0x0000,0x0010,0x0011,0x0020,0x2021,0x4001,0x6040,0x6041,0x6042
//	};
//	for(i=0;i<9;i++)
//	{
//		fprintf(stderr,"\n %04x-%d\n",OI_tmp[i],CalcOIDataLen(OI_tmp[i]));
//	}
//	CLASS_6015	class6015={};
//	////////////////////////////////////////////////////////////////////////////////test
//	memset(&class6015,0xee,sizeof(CLASS_6015));
//	class6015.csds.num = 1;
//	class6015.csds.csd[0].type=1;
//	class6015.csds.csd[0].csd.road.oad.OI =0x5004;
//	class6015.csds.csd[0].csd.road.oad.attflg = 0x02;
//	class6015.csds.csd[0].csd.road.oad.attrindex = 0x00;
//	class6015.csds.csd[0].csd.road.num = 3;
//	class6015.csds.csd[0].csd.road.oads[0].OI = 0x2021;
//	class6015.csds.csd[0].csd.road.oads[0].attflg = 0x02;
//	class6015.csds.csd[0].csd.road.oads[0].attrindex = 0x00;
//	class6015.csds.csd[0].csd.road.oads[1].OI = 0x0010;
//	class6015.csds.csd[0].csd.road.oads[1].attflg = 0x02;
//	class6015.csds.csd[0].csd.road.oads[1].attrindex = 0x00;
//	class6015.csds.csd[0].csd.road.oads[2].OI = 0x0020;
//	class6015.csds.csd[0].csd.road.oads[2].attflg = 0x02;
//	class6015.csds.csd[0].csd.road.oads[2].attrindex = 0x00;
//
//	class6015.data.type = 3;
//	class6015.data.data[0] = 1;
//	class6015.data.data[1] = 0x00;
//	class6015.data.data[2] = 0x0f;
//	fprintf(stderr,"\n---当前采集点数%d\n",CalcFreq(class6015));
//	class6015.data.type = 1;
//	class6015.data.data[0] = 1;
//	fprintf(stderr,"\n---当前采集点数%d\n",CalcFreq(class6015));
//
//
//
//
////	ReadNorData(ts,1,tsa,1);
////	SaveNorData(1,data_tmp2,sizeof(data_tmp2));
////	ReadNorData(ts,1,tsa,2);
////	SaveNorData(1,data_tmp3,sizeof(data_tmp3));
////	ReadNorData(ts,1,tsa,3);
//	return EXIT_SUCCESS;//退出
//}
