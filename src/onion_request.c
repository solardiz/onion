/*
	Onion HTTP server library
	Copyright (C) 2010 David Moreno Montero

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU Affero General Public License as
	published by the Free Software Foundation, either version 3 of the
	License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Affero General Public License for more details.

	You should have received a copy of the GNU Affero General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
	*/

#include <malloc.h>
#include <string.h>
#include <stdlib.h>
#include <libgen.h>

#include "onion_dict.h"
#include "onion_request.h"
#include "onion_handler.h"
#include "onion_server.h"
#include "onion_types_internal.h"

/// Creates a request
onion_request *onion_request_new(onion_server *server, void *socket){
	onion_request *req;
	req=malloc(sizeof(onion_request));
	memset(req,0,sizeof(onion_request));
	
	req->server=server;
	req->headers=onion_dict_new();
	req->socket=socket;
	req->buffer_pos=0;
	
	return req;
}

/// Deletes a request and all its data
void onion_request_free(onion_request *req){
	onion_dict_free(req->headers);
	
	if (req->fullpath)
		free(req->fullpath);
	if (req->query)
		onion_dict_free(req->query);
	
	free(req);
}

/// Partially fills a request. One line each time.
int onion_request_fill(onion_request *req, const char *data){
	//fprintf(stderr, "fill %s\n",data);
	if (!req->path){
		char method[16], url[256], version[16];
		sscanf(data,"%15s %255s %15s",method, url, version);

		/*
		fprintf(stderr, "'%s' %d\n", method, strcmp(method,"GET"));
		fprintf(stderr, "'%s'\n", url);
		fprintf(stderr, "'%s'\n", version);
		*/
		
		if (strcmp(method,"GET")==0)
			req->flags=OR_GET;
		else if (strcmp(method,"POST")==0)
			req->flags=OR_POST;
		else if (strcmp(method,"HEAD")==0)
			req->flags=OR_HEAD;
		else
			return 0; // Not valid method detected.

		if (strcmp(version,"HTTP/1.1")==0)
			req->flags|=OR_HTTP11;

		req->path=strndup(url,sizeof(url));
		req->fullpath=req->path;
	}
	else{
		char header[32], value[256];
		sscanf(data, "%31s", header);
		int i=0; 
		const char *p=&data[strlen(header)+1];
		while(*p && *p!='\n'){
			value[i++]=*p++;
			if (i==sizeof(value)-1){
				break;
			}
		}
		value[i]=0;
		header[strlen(header)-1]='\0'; // removes the :
		onion_dict_add(req->headers, header, value, OD_DUP_ALL);
	}
	return 1;
}

/**
 * @short Performs unquote inplace.
 *
 * It can be inplace as char position is always at least in the same on the destination than in the origin
 */
void onion_request_unquote(char *str){
	char *r=str;
	char *w=str;
	char tmp[3]={0,0,0};
	while (*r){
		if (*r == '%'){
			r++;
			tmp[0]=*r++;
			tmp[1]=*r;
			*w=strtol(tmp, (char **)NULL, 16);
		}
		else if (*r=='+'){
			*w=' ';
		}
		else{
			*w=*r;
		}
		r++;
		w++;
	}
	*w='\0';
}

/// Parses a query string.
int onion_request_parse_query(onion_request *req){
	if (!req->path)
		return 0;
	if (req->query) // already done
		return 1;
	
	char key[32], value[256];
	char cleanurl[256];
	int i=0;
	char *p=req->path;
	while(*p){
		//fprintf(stderr,"%d %c", *p, *p);
		if (*p=='?')
			break;
		cleanurl[i++]=*p;
		p++;
	}
	cleanurl[i++]='\0';
	onion_request_unquote(cleanurl);
	if (*p){ // There are querys.
		p++;
		req->query=onion_dict_new();
		int state=0;
		i=0;
		while(*p){
			if (state==0){
				if (*p=='='){
					key[i]='\0';
					state=1;
					i=-1;
				}
				else
					key[i]=*p;
			}
			else{
				if (*p=='&'){
					value[i]='\0';
					onion_request_unquote(key);
					onion_request_unquote(value);
					onion_dict_add(req->query, key, value, OD_DUP_ALL);
					state=0;
					i=-1;
				}
				else
					value[i]=*p;
			}
			
			p++;
			i++;
		}
		
		if (i!=0 || state!=0){
			if (state==0)
				key[i]='\0';
			else
				value[i]='\0';
			onion_request_unquote(key);
			onion_request_unquote(value);
			onion_dict_add(req->query, key, value, OD_DUP_ALL);
		}
	}
	free(req->fullpath);
	req->fullpath=strndup(cleanurl, sizeof(cleanurl));
	req->path=req->fullpath;
	return 1;
}


/**
 * @short Write some data into the request, and performs the query if necesary.
 *
 * This is where alomst all logic has place: it reads from the given data until it has all the headers
 * and launchs the root handler to perform the petition.
 */
int onion_request_write(onion_request *req, const char *data, unsigned int length){
	int i;
	char msgshown=0;
	for (i=0;i<length;i++){
		char c=data[i];
		if (c=='\n'){
			//fprintf(stderr,"newline\n");
			if (req->buffer_pos==0){ // If true, then headers are over. Do the processing.
				fprintf(stderr, "%s:%d GET %s\n",__FILE__,__LINE__,req->fullpath); // FIXME! This is no proper logging at all. Maybe a handler.

				onion_handler_handle(req->server->root_handler, req);
				return -i;
				// I do not stop as it might have more data: keep alive.
			}
			else{
				req->buffer[req->buffer_pos]='\0';
				onion_request_fill(req, req->buffer);
				req->buffer_pos=0;
			}
		}
		else if (c=='\r'){ // Just skip it when in headers
			//fprintf(stderr,"SKIP char %d\n",c);
		}
		else{
			//fprintf(stderr,"char %c %d\n",c,c);
			req->buffer[req->buffer_pos]=c;
			req->buffer_pos++;
			if (req->buffer_pos>=sizeof(req->buffer)){ // Overflow on headers
				req->buffer_pos--;
				if (!msgshown){
					fprintf(stderr,"onion / %s:%d Header too long for me (max header length (per header) %ld chars). Ignoring from that byte on to the end of this line. (%16s...)\n",basename(__FILE__),__LINE__, sizeof(req->buffer),req->buffer);
					fprintf(stderr,"onion / %s:%d Increase it at onion_request.h and recompile onion.\n",basename(__FILE__),__LINE__);
					msgshown=1;
				}
			}
		}
	}
	return i;
}

/// Returns a pointer to the string with the current path. Its a const and should not be trusted for long time.
const char *onion_request_get_path(onion_request *req){
	return req->path;
}

/// Moves the pointer inside fullpath to this new position, relative to current path.
void onion_request_advance_path(onion_request *req, int addtopos){
	req->path=&req->path[addtopos];
}

/// Gets a header data
const char *onion_request_get_header(onion_request *req, const char *header){
	return onion_dict_get(req->headers, header);
}

