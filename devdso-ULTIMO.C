/*
 *  dso
 */

#include	"u.h"
//#include	"libc.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

enum{
	Qdir,
	Qdata,
	Qcat,
	Qpar,
	Qmir,
	Qilv
};

enum{
	BLOCKSIZE = 8,  // 8 para depurar, 4KB o mas grande para uso
	DEVMAX = 50,
	ARGSMAX = 20,
	NFILESMAX = 10
};

/* Estructuras de datos */
typedef struct myfile MyFiles;

struct myfile{
	int id;
	char* name;
	int type;
	ulong size;
	ulong offset;
	Chan* files[NFILESMAX];
	Dir info[NFILESMAX];
	ulong mdate[NFILESMAX];
	int validos[NFILESMAX];
	int nfiles;
	int valido;
	QLock lock;
};


/* Fin Estr. Datos */

Dirtab dsotab[DEVMAX] = {
	".",	{Qdir, 0, QTDIR},	0,	0555,
	"ctl",	{Qdata, 0},		0,	0600,
};

int nfichtab = 0;
MyFiles ficheros[DEVMAX-2];
char* comandos[DEVMAX-2];
QLock fichLock;

#define	KBLINELEN	(3*NUMSIZE+1)	/* t code val\n */

static Chan *
dsoattach(char *spec)
{
	return devattach(L'Q', spec);
}

static Walkqid*
dsowalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, dsotab, nfichtab+2, devgen);
}

static int
dsostat(Chan *c, uchar *dp, int n)
{
	return devstat(c, dp, n, dsotab, nfichtab+2, devgen);
}

static Chan*
dsoopen(Chan *c, int omode)
{
	if(!iseve())
		error(Eperm);

	// SWITCH TIPOS ARCHIVO

	return devopen(c, omode, dsotab, nfichtab+2, devgen);
}

static void
dsoclose(Chan *c)
{
	if(c->aux){
		free(c->aux);
		c->aux = nil;
	}
	// SWITCH TIPOS ARCHIVO
}


char* usage(){
	int i;
	char* aux[] = {
		"\nDriver de Particion, Interleaving, Mirror y Concatenacion de archivos:",
		"\nComandos:\n\t+ <target> <fich1> ... <fichn> -",
		" Concatena en <target> los ficheros dados\n\tp ",
		"<target> <offset> <size> <source> -\n\t\tParticiona",
		" <source> en <target> a partir del <offset> dado y",
		" con tamaño <size>\n\ti <target> <fich1> ... <fichn>",
		" - Monta un Interleaving en <target> con los ficheros",
		" dados\n\tm <target> <fich1> ... <fichn> - Monta un",
		" Mirror en <target> con los ficheros dados\nEl numero",
		" maximo de ficheros por dispositivo es de %d\n",
		"El numero de dispositivos actuales es %d de un maximo",
		" de %d\nDriver hecho por Luis Gasco Poderoso\n\n"
		};
	int t;
	char* text = (char*)malloc(1024);

	t = sizeof(aux)/sizeof(char*);
	for(i = 0; i < t; i++){
		strcat(text,aux[i]);
	}

	sprint(text,text,NFILESMAX,nfichtab,DEVMAX-2);

	return text;
}

// Optiene la fecha de ultima modificacion de c y la compara con
// la almacenada en d
// 1: Modificado o corrupto
// 0: No modificado
int
checkLastModified(Chan* c,ulong d){
	int tamb = 1024;
	Dir fs;
	uchar* buffer = (uchar*)malloc(sizeof(uchar)*tamb);

	if(!waserror()){
		devtab[c->type]->stat(c,buffer,tamb);
		convM2D(buffer, tamb, &fs, 0);

		poperror();

		if(d != fs.mtime)
			return 1;
		return 0;
	}

	// Fue error, por lo que algo malo paso
	return 1;
}

// Comprobando la validez
static int
checkFiles(MyFiles *f){
	int nok, i;

	nok = 0;

	for(i = 0; i < f->nfiles; i++){
		//print("%ld\n",f->mdate[i]);
		if(checkLastModified(f->files[i],f->mdate[i]))
			f->validos[i] = 0;
		nok += f->validos[i];
	}

	return (f->nfiles - nok); // Si hay al menos  un invalido, sera !=0
}

static int
existDev(char* name){
	if(!waserror()){
		namec(name,Aopen,ORDWR,0);
		poperror();
		return 1;
	}

	return 0;
}

static MyFiles*
getMyFile(Chan *c){
	int i = c->qid.path - 2; // Hay un desfase de 2 ficheros, raiz y ctl
	if(i < 0 || i >= nfichtab)
		print("BUG: Fuera del array??");

	return  &ficheros[i];
}

void
setSize(Chan* c){
	int i = c->qid.path - 2; // Hay un desfase de 2 ficheros, raiz y ctl
	if(i < 0 || i >= nfichtab)
		print("BUG_SIZE: Fuera del array??");

	//dsotab[i+2].length = ficheros[i].size;
	ficheros[i].size = dsotab[i+2].length;
}


/* Funciones para lectura de dispositivos */

static long
dsopartread(MyFiles *f, void *a, long n, vlong offset){
	Chan* file = f->files[0];

	if (offset >= f->size)
		return 0;
	if ((offset + n) > f->size)
		n = f->size - offset;
	if (n == 0)
		return 0;

	return devtab[file->type]->read(file, a, n, (offset + f->offset));
}

static long
dsoconcatread(MyFiles *f, void *a, long n, vlong offset){
	int nfiles = f->nfiles;
	long leido = 0, aux_leido;
	vlong newoff = offset;
	Dir* fs;
	int tamb = 1024;
	int i = 0, j;
	long toread;
	uchar* buffer = (uchar*)malloc(sizeof(uchar)*tamb);

	// Posicionamos el offset
	
	Chan* file = f->files[i];

	if (offset >= f->size)
		return 0;
	if ((offset + n) > f->size)
		n = f->size - offset;
	if (n == 0)
		return 0;

	fs = &(f->info[i]);

	devtab[file->type]->stat(file,buffer,tamb);
	convM2D(buffer, tamb, fs, 0);

	for(; (newoff > fs->length) && (i < nfiles);){
		newoff -= fs->length;
		file = f->files[++i];
		devtab[file->type]->stat(file,buffer,tamb);
		convM2D(buffer, tamb, fs, 0);
	}

	// Vamos leyendo hasta terminar
	if(i < nfiles){
		toread = fs->length - newoff;
		leido += devtab[file->type]->read(file, a, toread, newoff);
		a = (char*)a + leido;
		for(j = i+1; (leido < n) && (j < nfiles); j++){
			file = f->files[j];
			fs = &(f->info[j]);
			devtab[file->type]->stat(file,buffer,tamb);
			convM2D(buffer, tamb, fs, 0);	

			if((leido + fs->length) < n)
				toread = fs->length;
			else
				toread = n-leido; 

			aux_leido = devtab[file->type]->read(file, a, toread, 0);
			
			a = (char*)a + aux_leido;
			leido += aux_leido;
		}
	}

	return leido;
}

static long
dsomirrorread(MyFiles *f, void *a, long n, vlong offset){
	int i;

	for(i = 0; i < f->nfiles; i++){
		if(f->validos[i]){
			Chan* file = f->files[i];

			if (offset >= f->size)
				return 0;
			if ((offset + n) > f->size)
				n = f->size - offset;
			if (n == 0)
				return 0;

			return devtab[file->type]->read(file, a, n, (offset + f->offset));
		}else{
			print("El fichero %d del mirror %s esta corrupto\n", i+1, f->name);
		}
	}

	print("No quedan ficheros integros en %s\n",f->name);

	return 0;
}

static long
dsointerlread(MyFiles *f, void *a, long n, vlong offset){
	long r = 0, aux_r;
	long toread;
	int i;
	vlong os = 0;
	vlong  num_bloque, bloque_actual;
	Chan* file;

	if (offset >= f->size)
		return 0;
	if ((offset + n) > f->size)
		n = f->size - offset;
	if (n == 0)
		return 0;

	// Determinar donde comenzamos a leer
	num_bloque = offset / BLOCKSIZE;
	os = offset % BLOCKSIZE;
	// Cuanto leer en primera iteracion
	toread = BLOCKSIZE - os;

	while(r < n){
		i = num_bloque % f->nfiles; // Fichero del que leer
		file = f->files[i];
		bloque_actual = num_bloque / f->nfiles; // De ese fichero, ¿que bloque?
		os += bloque_actual * BLOCKSIZE; // Offset para el fichero actual

		if(n - r < toread)
			toread = n - r;		

		aux_r = devtab[file->type]->read(file, a, toread, os);
		a  = (char*)a + aux_r;
		r += aux_r;

		os = 0;
		num_bloque++;
		toread = BLOCKSIZE;
	}

	return r;
}

char*
getComandos(void){
	int i;
	char* ret = (char*)malloc(500*DEVMAX);

	for(i = 0; i < nfichtab; i++){
		strcat(ret,comandos[i]);
	}

	return ret;
}

static long
dsoread(Chan *c, void *a, long n, vlong offset)
{
	MyFiles *f;
	char* s;

	if(c->qid.type == QTDIR)
		return devdirread(c, a, n, dsotab, nfichtab+2, devgen);

	if(c->qid.path == Qdata){
		//s = usage();
		s = getComandos();
		return readstr(offset, a, n, s);
	}

	f = getMyFile(c);

	qlock(&f->lock); 

	if(!f->valido || checkFiles(f) && (f->type != Qmir)){
		f->valido = 0;
		print("Fichero corrupto: %s\n",f->name);
		//qunlock(&f->lock);
		//error("El fichero esta corrupto");
	}

	switch(f->type){
		case Qcat:
			n = dsoconcatread(f, a, n, offset);
			break;
		case Qpar:
			n = dsopartread(f, a, n, offset);
			break;
		case Qmir:
			n = dsomirrorread(f, a, n,offset);
			break;
		case Qilv:
			n = dsointerlread(f, a, n, offset);
			break;
		default:
			print("BUG: f->type desconocido\n");
			n = 0;
	}

	qunlock(&f->lock);

	return n;
}

/* Al insertar nuevos dispositivos, estos no son
     visibles hasta que estan completamente insertados
     (nfichtab++) por lo que no hay problemas de
     concurrencia de que alguien lea mientras se
     inserta un nuevo dispositivo la tabla, la concurrencia
     en 2 inserciones simultaneas se trata con QLock*/

long
dsoconcat(char** args, int nargs){
	ulong size;
	int i;
	Chan* actual;
	char* target = args[1];

	int tamb = 1024;
	uchar* buffer = (uchar*)malloc(sizeof(uchar)*tamb);

	if(existDev(target)){
		qunlock(&fichLock);
		error("#Q: El dispositivo ya existe");
	}

	if(nfichtab >= DEVMAX-2){
		qunlock(&fichLock);
		error("#Q: No se permiten mas dispositivos");
	}

	/* Introducir en estructuras */
	
	if(!waserror()){
		ficheros[nfichtab].id = nfichtab;

		ficheros[nfichtab].name = nil;
		kstrdup(&ficheros[nfichtab].name,target);

		ficheros[nfichtab].type = Qcat;

		ficheros[nfichtab].nfiles = nargs-2;
	
		size = 0;

		for(i = 0; i < nargs-2; i++){
			actual = namec(args[i+2],Aopen,ORDWR,0);
			devtab[actual->type]->stat(actual,buffer,tamb);
			convM2D(buffer, tamb, &(ficheros[nfichtab].info[i]), 0);
			size += ficheros[nfichtab].info[i].length;
			ficheros[nfichtab].files[i] = actual;
			ficheros[nfichtab].validos[i] = 1;
			ficheros[nfichtab].mdate[i] = ficheros[nfichtab].info[i].mtime;
			//print("%ld\n",ficheros[nfichtab].mdate[i]);
		}
		
		strcpy(dsotab[nfichtab+2].name, ficheros[nfichtab].name);
		dsotab[nfichtab+2].qid.path = nfichtab+2; //donde esta en mi estructura
		dsotab[nfichtab+2].length = size;
		dsotab[nfichtab+2].perm = 0600;

		ficheros[nfichtab].size = size;
		ficheros[nfichtab].valido = 1;
		
		poperror();
		return nfichtab++;
	}

	qunlock(&fichLock);
	error("#Q: Asegurese de que existen los ficheros origen");

	return -1;
}

long
dsopart(char** args, int nargs){
	int offset;
	ulong size, size_file;
	char* target = args[1];
	Chan* actual;

	int tamb = 1024;
	uchar* buffer = (uchar*)malloc(sizeof(uchar)*tamb);

	if(nargs != 5){
		print("BUG: No deberia llamarse sin comprobarse  antes");
		return -1;
	}

	offset = atoi(args[2]);
	size = atoi(args[3]);

	if(existDev(target)){
		qunlock(&fichLock);
		error("#Q: El dispositivo ya existe");
	}

	if(nfichtab >= DEVMAX-2){
		qunlock(&fichLock);
		error("#Q: No se permiten mas dispositivos");
	}

	/* Introducir en estructuras */
	
	if(!waserror()){
		ficheros[nfichtab].id = nfichtab;

		ficheros[nfichtab].name = nil;
		kstrdup(&ficheros[nfichtab].name,target);

		ficheros[nfichtab].type = Qpar;

		ficheros[nfichtab].nfiles = 1;

		actual = namec(args[4],Aopen,ORDWR,0);
		devtab[actual->type]->stat(actual,buffer,tamb);
		
		convM2D(buffer, tamb, &(ficheros[nfichtab].info[0]), 0);
		size_file = ficheros[nfichtab].info[0].length;

		ficheros[nfichtab].files[0] = actual;
		ficheros[nfichtab].validos[0] = 1;
		ficheros[nfichtab].mdate[0] = ficheros[nfichtab].info[0].mtime;

		if((size+offset) > size_file){
			poperror();
			qunlock(&fichLock);
			error("#Q: target size + offset cannot be greater than file size\n");
			return -1; 
		}else{
			strcpy(dsotab[nfichtab+2].name, ficheros[nfichtab].name);
			dsotab[nfichtab+2].qid.path = nfichtab+2; //donde esta en mi estructura
			dsotab[nfichtab+2].length = size;
			dsotab[nfichtab+2].perm = 0600;

			ficheros[nfichtab].size = size;
			ficheros[nfichtab].offset = offset;
			ficheros[nfichtab].valido = 1;
		
			poperror();
			return nfichtab++;
		}
	}

	qunlock(&fichLock);
	error("#Q: Compruebe que existe el fichero origen");

	return -1;
}

long
dsomirror(char** args, int nargs){
	int i;
	ulong size, size_aux;
	
	char* target = args[1];
	Chan* actual;
	
	int tamb = 1024;
	uchar* buffer = (uchar*)malloc(sizeof(uchar)*tamb);
	
	if(existDev(target)){
		qunlock(&fichLock);
		error("#Q: El dispositivo ya existe");
	}

	if(nfichtab >= DEVMAX-2){
		qunlock(&fichLock);
		error("#Q: No se permiten mas dispositivos");
	}

	/* Introducir en estructuras */
	
	if(!waserror()){
		ficheros[nfichtab].id = nfichtab;

		ficheros[nfichtab].name = nil;
		kstrdup(&ficheros[nfichtab].name,target);

		ficheros[nfichtab].type = Qmir;

		ficheros[nfichtab].nfiles = nargs-2;
	
		size = -1; // Max ulong

		for(i = 0; i < nargs-2; i++){
			actual = namec(args[i+2],Aopen,ORDWR,0);
			devtab[actual->type]->stat(actual,buffer,tamb);

			convM2D(buffer, tamb, &(ficheros[nfichtab].info[i]), 0);
			size_aux = ficheros[nfichtab].info[i].length;
			if(size_aux < size){
				size = size_aux;
			}

			ficheros[nfichtab].files[i] = actual;
			ficheros[nfichtab].validos[i] = 1;
			ficheros[nfichtab].mdate[i] = ficheros[nfichtab].info[i].mtime;
		}
		
		strcpy(dsotab[nfichtab+2].name, ficheros[nfichtab].name);
		dsotab[nfichtab+2].qid.path = nfichtab+2; //donde esta en mi estructura
		dsotab[nfichtab+2].length = size;
		dsotab[nfichtab+2].perm = 0600;

		ficheros[nfichtab].size = size;
		ficheros[nfichtab].valido = 1;
		
		poperror();
		return nfichtab++;
	}

	qunlock(&fichLock);
	error("#Q: Asegurese de que existen los ficheros origen");

	return -1;
}

long
dsointerl(char** args, int nargs){
	int i;
	ulong size;
	char* target = args[1];
	Chan* actual;
	
	int tamb = 1024;
	uchar* buffer = (uchar*)malloc(sizeof(uchar)*tamb);
	
	if(existDev(target)){
		qunlock(&fichLock);
		error("#Q: El dispositivo ya existe");
	}

	if(nfichtab >= DEVMAX-2){
		qunlock(&fichLock);
		error("#Q: No se permiten mas dispositivos");
	}

	/* Introducir en estructuras */
	
	if(!waserror()){
		ficheros[nfichtab].id = nfichtab;

		ficheros[nfichtab].name = nil;
		kstrdup(&ficheros[nfichtab].name,target);

		ficheros[nfichtab].type = Qilv;

		ficheros[nfichtab].nfiles = nargs-2;
	
		size = 0;

		for(i = 0; i < nargs-2; i++){
			actual = namec(args[i+2],Aopen,ORDWR,0);
			devtab[actual->type]->stat(actual,buffer,tamb);
			convM2D(buffer, tamb, &(ficheros[nfichtab].info[i]), 0);
			size += ficheros[nfichtab].info[i].length;
			ficheros[nfichtab].files[i] = actual;
			ficheros[nfichtab].validos[i] = 1;
			ficheros[nfichtab].mdate[i] = ficheros[nfichtab].info[i].mtime;
		}
		
		strcpy(dsotab[nfichtab+2].name, ficheros[nfichtab].name);
		dsotab[nfichtab+2].qid.path = nfichtab+2; //donde esta en mi estructura
		dsotab[nfichtab+2].length = size;
		dsotab[nfichtab+2].perm = 0600;

		ficheros[nfichtab].size = size;
		ficheros[nfichtab].valido = 1;

		poperror();
		return nfichtab++;
	}

	qunlock(&fichLock);
	error("#Q: Asegurese de que existen los ficheros origen");

	return -1;
}

long
dsoaction(char** args, int nargs){
	long r = 0;

	// Comprobar que no introdujo '/'
	if(nargs > 1 && strcspn(args[1],"//") < strlen(args[1]))
		error("#Q: Debe dar un nombre de dispositivo, no una ruta");

	// Bloqueo al introducir nuevo dispositivos
	qlock(&fichLock);

	if(strcmp(args[0],"+") == 0){
		if(nargs > 3)
			r = dsoconcat(args,nargs);
		else{
			qunlock(&fichLock);
			error("#Q: USO: + <dest> <origen1> ... <origenN>");
		}
	}else
	if(strcmp(args[0],"p") == 0){
		if(nargs == 5)
			r = dsopart(args,nargs);
		else{
			qunlock(&fichLock);
			error("#Q: USO: p <dest> <offset> <size> <origen>");
		}
	}else
	if(strcmp(args[0],"m") == 0){
		if(nargs > 3)
			r = dsomirror(args,nargs);
		else{
			qunlock(&fichLock);
			error("#Q: USO: m <dest> <origen1> ... <origenN>");
		}
	}else
	if(strcmp(args[0],"i") == 0){
		if(nargs > 3)
			r = dsointerl(args,nargs);
		else{
			qunlock(&fichLock);
			error("#Q: USO: i <dest> <origen1> ... <origenN>");
		}
	}else{
		qunlock(&fichLock);
		error("#Q: Comando invalido\nAyuda con \"cat \'#Q\'/ctl\"\n");
	}

	qunlock(&fichLock);
	return r;
}

/* Funciones de escritura en dispositivos */

static long
dsoconcatwrite(MyFiles *f, void *a, long n, vlong offset){
	int nfiles = f->nfiles;
	long escrito = 0;
	long newsize = offset;
	vlong newoff = offset;
	Dir* fs;
	int tamb = 1024;
	int i = 0, j;
	long towrite;
	uchar* buffer = (uchar*)malloc(sizeof(uchar)*tamb);

	// Posicionamos el offset
	
	Chan* file = f->files[i];

	/*if (offset >= f->size)
		return 0;
	if ((offset + n) > f->size)
		n = f->size - offset;*/
	if (n == 0)
		return 0;

	fs = &(f->info[i]);

	devtab[file->type]->stat(file,buffer,tamb);
	convM2D(buffer, tamb, fs, 0);

	for(; (newoff > fs->length) && (i < nfiles);){
		newoff -= fs->length;
		file = f->files[++i];
		devtab[file->type]->stat(file,buffer,tamb);
		convM2D(buffer, tamb, fs, 0);
	}

	// Vamos leyendo hasta terminar
	if(i < nfiles-1){
		towrite = fs->length - newoff;
		escrito += devtab[file->type]->write(file, a, towrite, newoff);
		a = (char*)a + escrito;

		fs = &(f->info[i]);

		devtab[file->type]->stat(file,buffer,tamb);
		convM2D(buffer, tamb, fs, 0);
		f->mdate[i] = fs->mtime;

		for(j = i+1; (escrito < n) && (j < nfiles); j++){
			file = f->files[j];
			devtab[file->type]->stat(file,buffer,tamb);
			convM2D(buffer, tamb, fs, 0);	

			if(((escrito + fs->length) < n) && (j < nfiles-1))
				towrite = fs->length;
			else
				towrite = n-escrito; 

			escrito += devtab[file->type]->write(file, a, towrite, 0);
			a = (char*)a + escrito;

			fs = &(f->info[j]);

			devtab[file->type]->stat(file,buffer,tamb);
			convM2D(buffer, tamb, fs, 0);
			f->mdate[j] = fs->mtime;
		}
	}else if(i == nfiles-1){
		escrito = devtab[file->type]->write(file, a, n, 0);
	}

/*
	newsize += escrito;

	if(newsize > f->size)
		f->size = newsize;
*/

	return escrito;
}

static long
dsopartwrite(MyFiles *f, void *a, long n, vlong offset){
	Chan* file = f->files[0];
	int tamb = 1024;
	Dir* fs;
	int r;
	uchar* buffer = (uchar*)malloc(sizeof(uchar)*tamb);

	if (offset >= f->size)
		return 0;
	if ((offset + n) > f->size)
		n = f->size - offset;
	if (n == 0)
		return 0;

	r = devtab[file->type]->write(file, a, n, (offset + f->offset));

	fs = &(f->info[0]);

	devtab[file->type]->stat(file,buffer,tamb);
	convM2D(buffer, tamb, fs, 0);
	f->mdate[0] = fs->mtime;

	return r;
}

static long
dsomirrorwrite(MyFiles *f, void *a, long n, vlong offset){
	int i, w = 0;
	long r = 0;
	Dir* fs;
	int tamb = 1024;
	uchar* buffer = (uchar*)malloc(sizeof(uchar)*tamb);

	for(i = 0; i < f->nfiles; i++){
		if(f->validos[i]){
			Chan* file = f->files[i];

			/*if (offset >= f->size)
				return 0;
			if ((offset + n) > f->size)
				n = f->size - offset;*/
			if (n == 0)
				return 0;

			r = devtab[file->type]->write(file, a, n, (offset + f->offset));

			fs = &(f->info[i]);

			devtab[file->type]->stat(file,buffer,tamb);
			convM2D(buffer, tamb, fs, 0);
			f->mdate[i] = fs->mtime;	
			w = 1;
		}else{
			print("El fichero %d del mirror %s esta corrupto\n", i+1, f->name);
		}
	}

	if(!w){
		print("No quedan ficheros integros en %s\n",f->name);
	}
/*
	if(offset+r > f->size)
		f->size = offset + r;
*/
	return r;
}

static long
dsointerlwrite(MyFiles *f, void *a, long n, vlong offset){
	long r = 0, aux_r;
	long towrite;
	int i;
	vlong os = 0;
	vlong  num_bloque, bloque_actual;
	Chan* file;
	Dir* fs;
	int tamb = 1024;
	uchar* buffer = (uchar*)malloc(sizeof(uchar)*tamb);

	/*if (offset >= f->size)
		return 0;
	if ((offset + n) > f->size)
		n = f->size - offset;*/
	if (n == 0)
		return 0;

	// Determinar donde comenzamos a escribir
	num_bloque = offset / BLOCKSIZE;
	os = offset % BLOCKSIZE;
	// Cuanto escribir en primera iteracion
	towrite = BLOCKSIZE - os;

	while(r < n){
		i = num_bloque % f->nfiles; // Fichero del que leer
		if(f->validos[i]){
			file = f->files[i];
			bloque_actual = num_bloque / f->nfiles; // De ese fichero, ¿que bloque?
			os += bloque_actual * BLOCKSIZE; // Offset para el fichero actual

			if(n - r < towrite)
				towrite = n - r;		

			aux_r = devtab[file->type]->write(file, a, towrite, os);
			a  = (char*)a + aux_r;
			r += aux_r;

			fs = &(f->info[i]);

			devtab[file->type]->stat(file,buffer,tamb);
			convM2D(buffer, tamb, fs, 0);
			f->mdate[i] = fs->mtime;	

			os = 0;
			num_bloque++;
			towrite = BLOCKSIZE;
			
		}else{
			Dir* fs = &f->info[i];
			print("El fichero %s del interleaving %s esta corrupto\n",fs->name,f->name);
		}
	}
/*
	if(offset+r > f->size)
		f->size = offset + r;
*/
	return r;
}

void
addCommand(char*  cmd){
	comandos[nfichtab-1] = nil;  // Ya se habia incrementado
	kstrdup(&comandos[nfichtab-1], cmd);
}

static long
dsowrite(Chan *c, void *a, long n, vlong offset)
{
	MyFiles *f;
	char *b = a;
	int i;
	char *line = (char*)malloc(n+1);
	char* cmd = nil;
	char *args[ARGSMAX];
	int nargs;
	long r = 0;

	for(i = 0; i < n; i++){
		line[i] = b[i];
	}

	line[n] = '\0';

	if(c->qid.type == QTDIR){
		error(Eperm);
	}

	if(c->qid.path == Qdata){
		kstrdup(&cmd,line);
		nargs = tokenize(line, args, ARGSMAX);
		r = dsoaction(args,nargs);

		if(r >= 0){
			addCommand(cmd);
			return n;
		}

		return -1;
	}

	f = getMyFile(c);

	qlock(&f->lock);

	if(f->valido && !checkFiles(f) || (f->type == Qmir)){
		switch(f->type){
			case Qcat:
				r = dsoconcatwrite(f, a, n, offset);
				setSize(c);
				break;
			case Qpar:
				r = dsopartwrite(f, a, n, offset);
				break;
			case Qmir:
				r = dsomirrorwrite(f, a, n, offset);
				setSize(c);
				break;
			case Qilv:
				r = dsointerlwrite(f, a, n, offset);
				setSize(c);
				break;
			default:
				print("BUG_WRITE: f->type desconocido\n");
				r = -1;
		}
	}else{
		f->valido = 0;
		print("Fichero corrupto: %s\n",f->name);
		qunlock(&f->lock);
		error("#Q: El fichero esta corrupto");
	}

	qunlock(&f->lock);

	return r;
}

Dev dsodevtab = {
	L'Q',
	"dso",

	devreset,
	devinit,
	devshutdown,
	dsoattach,
	dsowalk,
	dsostat,
	dsoopen,
	devcreate,
	dsoclose,
	dsoread,
	devbread,
	dsowrite,	//Interfaz
	devbwrite,
	devremove,
	devwstat,
};
