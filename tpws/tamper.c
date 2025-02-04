#define _GNU_SOURCE

#include "tamper.h"
#include "params.h"
#include "hostlist.h"
#include "protocol.h"
#include "helpers.h"
#include <string.h>
#include <stdio.h>

// pHost points to "Host: ..."
bool find_host(uint8_t **pHost,uint8_t *buf,size_t bs)
{
	if (!*pHost)
	{
		*pHost = memmem(buf, bs, "\nHost:", 6);
		if (*pHost)
		{
			(*pHost)++;
			VPRINT("Found Host: at pos %td",*pHost - buf)
		}
	}
	return !!*pHost;
}

static const char *http_methods[] = { "GET /","POST /","HEAD /","OPTIONS /","PUT /","DELETE /","CONNECT /","TRACE /",NULL };
// segment buffer has at least 5 extra bytes to extend data block
void modify_tcp_segment(uint8_t *segment,size_t segment_buffer_size,size_t *size,size_t *split_pos)
{
	uint8_t *p, *pp, *pHost = NULL;
	size_t method_len = 0, pos;
	const char **method;
	bool bIsHttp = false, bBypass = false;
	char bRemovedHostSpace = 0;
	char *pc, Host[128];
	
	*split_pos=0;

	for (method = http_methods; *method; method++)
	{
		method_len = strlen(*method);
		if (method_len <= *size && !memcmp(segment, *method, method_len))
		{
			bIsHttp = true;
			method_len -= 2; // "GET /" => "GET"
			break;
		}
	}
	if (bIsHttp)
	{
		VPRINT("Data block looks like http request start : %s", *method)
		// cpu saving : we search host only if and when required. we do not research host every time we need its position
		if ((params.hostlist || params.hostlist_exclude) && find_host(&pHost,segment,*size))
		{
			p = pHost + 5;
			while (p < (segment + *size) && (*p == ' ' || *p == '\t')) p++;
			pp = p;
			while (pp < (segment + *size) && (pp - p) < (sizeof(Host) - 1) && *pp != '\r' && *pp != '\n') pp++;
			memcpy(Host, p, pp - p);
			Host[pp - p] = '\0';
			VPRINT("Requested Host is : %s", Host)
			for(pc = Host; *pc; pc++) *pc=tolower(*pc);
			bBypass = !HostlistCheck(params.hostlist, params.hostlist_exclude, Host);
		}
		if (!bBypass)
		{
			if (params.unixeol)
			{
				p = pp = segment;
				while ((p = memmem(p, segment + *size - p, "\r\n", 2)))
				{
					*p = '\n'; p++;
					memmove(p, p + 1, segment + *size - p - 1);
					(*size)--;
					if (pp == (p - 1))
					{
						// probably end of http headers
						VPRINT("Found double EOL at pos %td. Stop replacing.", pp - segment)
						break;
					}
					pp = p;
				}
				pHost = NULL; // invalidate
			}
			if (params.methodeol && (*size+1+!params.unixeol)<=segment_buffer_size)
			{
				VPRINT("Adding EOL before method")
				if (params.unixeol)
				{
					memmove(segment + 1, segment, *size);
					(*size)++;;
					segment[0] = '\n';
				}
				else
				{
					memmove(segment + 2, segment, *size);
					*size += 2;
					segment[0] = '\r';
					segment[1] = '\n';
				}
				pHost = NULL; // invalidate
			}
			if (params.methodspace && *size<segment_buffer_size)
			{
				// we only work with data blocks looking as HTTP query, so method is at the beginning
				VPRINT("Adding extra space after method")
				p = segment + method_len + 1;
				pos = method_len + 1;
				memmove(p + 1, p, *size - pos);
				*p = ' '; // insert extra space
				(*size)++; // block will grow by 1 byte
				if (pHost) pHost++; // Host: position will move by 1 byte
			}
			if ((params.hostdot || params.hosttab) && *size<segment_buffer_size && find_host(&pHost,segment,*size))
			{
				p = pHost + 5;
				while (p < (segment + *size) && *p != '\r' && *p != '\n') p++;
				if (p < (segment + *size))
				{
					pos = p - segment;
					VPRINT("Adding %s to host name at pos %zu", params.hostdot ? "dot" : "tab", pos)
					memmove(p + 1, p, *size - pos);
					*p = params.hostdot ? '.' : '\t'; // insert dot or tab
					(*size)++; // block will grow by 1 byte
				}
			}
			if (params.domcase && find_host(&pHost,segment,*size))
			{
				p = pHost + 5;
				pos = p - segment;
				VPRINT("Mixing domain case at pos %zu",pos)
				for (; p < (segment + *size) && *p != '\r' && *p != '\n'; p++)
					*p = (((size_t)p) & 1) ? tolower(*p) : toupper(*p);
			}
			if (params.hostnospace && find_host(&pHost,segment,*size) && (pHost+5)<(segment+*size) && pHost[5] == ' ')
			{
				p = pHost + 6;
				pos = p - segment;
				VPRINT("Removing space before host name at pos %zu", pos)
				memmove(p - 1, p, *size - pos);
				(*size)--; // block will shrink by 1 byte
				bRemovedHostSpace = 1;
			}
			if (params.hostcase && find_host(&pHost,segment,*size))
			{
				VPRINT("Changing 'Host:' => '%c%c%c%c:' at pos %td", params.hostspell[0], params.hostspell[1], params.hostspell[2], params.hostspell[3], pHost - segment)
				memcpy(pHost, params.hostspell, 4);
			}
			if (params.hostpad && find_host(&pHost,segment,*size))
			{
				//  add :  XXXXX: <padding?[\r\n|\n]
				char s[8];
				size_t hsize = params.unixeol ? 8 : 9;
				size_t hostpad = params.hostpad<hsize ? hsize : params.hostpad;

				if ((hsize+*size)>segment_buffer_size)
					VPRINT("could not add host padding : buffer too small")
				else
				{
					if ((hostpad+*size)>segment_buffer_size)
					{
						hostpad=segment_buffer_size-*size;
						VPRINT("host padding reduced to %zu bytes : buffer too small", hostpad)
					}
					else
						VPRINT("host padding with %zu bytes", hostpad)
					
					p = pHost;
					pos = p - segment;
					memmove(p + hostpad, p, *size - pos);
					(*size) += hostpad;
					while(hostpad)
					{
						#define MAX_HDR_SIZE	2048
						size_t padsize = hostpad > hsize ? hostpad-hsize : 0;
						if (padsize>MAX_HDR_SIZE) padsize=MAX_HDR_SIZE;
						// if next header would be too small then add extra padding to the current one
						if ((hostpad-padsize-hsize)<hsize) padsize+=hostpad-padsize-hsize;
						snprintf(s,sizeof(s),"%c%04x: ", 'a'+rand()%('z'-'a'+1), rand() & 0xFFFF);
						memcpy(p,s,7);
						p+=7;
						memset(p,'a'+rand()%('z'-'a'+1),padsize);
						p+=padsize;
						if (params.unixeol)
							*p++='\n';
						else
						{
							*p++='\r';
							*p++='\n';
						}
						hostpad-=hsize+padsize;
					}
					pHost = NULL; // invalidate
				}
			}
			switch (params.split_http_req)
			{
				case split_method:
					*split_pos = method_len - 1 + params.methodeol + (params.methodeol && !params.unixeol);
					break;
				case split_host:
					if (find_host(&pHost,segment,*size))
						*split_pos = pHost + 6 - bRemovedHostSpace - segment;
					break;
				default:
					if (params.split_pos < *size) *split_pos = params.split_pos;
			}
		}
		else
		{
			VPRINT("Not acting on this request")
		}
		return;
	}
	
	if (IsTLSClientHello(segment,*size))
	{
		char host[256];
		size_t tpos=0,elen;
		const uint8_t *ext;

		VPRINT("packet contains TLS ClientHello")
		// we need host only if hostlist is present
		if ((params.hostlist || params.hostlist_exclude) && TLSHelloExtractHost((uint8_t*)segment,*size,host,sizeof(host)))
		{
			VPRINT("hostname: %s",host)
			if (!HostlistCheck(params.hostlist, params.hostlist_exclude, host))
			{
				VPRINT("Not acting on this request")
				return;
			}
		}
		switch(params.tlsrec)
		{
			case tlsrec_sni:
				if (TLSFindExt(segment,*size,0,&ext,&elen))
					tpos = ext-segment+1; // between typical 1st and 2nd char of hostname
				break;
			case tlsrec_pos:
				tpos = params.tlsrec_pos;
				break;
			default:
				break;
		}
		if (tpos)
		{
			// construct 2 TLS records from one
			uint16_t l = pntoh16(segment+3); // length
			if (l>=2)
			{
				// length is checked in IsTLSClientHello and cannot exceed buffer size
				if (tpos>=l) tpos=1;
				VPRINT("making 2 TLS records at pos %zu",tpos)
				memmove(segment+5+tpos+5,segment+5+tpos,*size-(5+tpos));
				segment[5+tpos] = segment[0];
				segment[5+tpos+1] = segment[1];
				segment[5+tpos+2] = segment[2];
				phton16(segment+5+tpos+3,l-tpos);
				phton16(segment+3,tpos);
				*size += 5;
			}
		}
		
		if (params.split_pos < *size)
			*split_pos = params.split_pos;
		return;
	}
	
	if (params.split_any_protocol && params.split_pos < *size)
		*split_pos = params.split_pos;
}
