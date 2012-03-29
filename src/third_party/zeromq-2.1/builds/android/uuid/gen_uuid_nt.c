/*
 * gen_uuid_nt.c -- Use NT api to generate uuid
 *
 * Written by Andrey Shedel (andreys@ns.cr.cyco.com)
 */


#include "uuidP.h"

#pragma warning(push,4)

#pragma comment(lib, "ntdll.lib")

//
// Here is a nice example why it's not a good idea
// to use native API in ordinary applications.
// Number of parameters in function below was changed from 3 to 4
// for NT5.
//
//
// NTSYSAPI
// NTSTATUS
// NTAPI
// NtAllocateUuids(
//     OUT PULONG p1,
//     OUT PULONG p2,
//     OUT PULONG p3,
//     OUT PUCHAR Seed // 6 bytes
//   );
//
//

unsigned long
__stdcall
NtAllocateUuids(
   void* p1,  // 8 bytes
   void* p2,  // 4 bytes
   void* p3   // 4 bytes
   );

typedef
unsigned long
(__stdcall*
NtAllocateUuids_2000)(
   void* p1,  // 8 bytes
   void* p2,  // 4 bytes
   void* p3,  // 4 bytes
   void* seed // 6 bytes
   );



//
// Nice, but instead of including ntddk.h ot winnt.h
// I should define it here because they MISSED __stdcall in those headers.
//

__declspec(dllimport)
struct _TEB*
__stdcall
NtCurrentTeb(void);


//
// The only way to get version information from the system is to examine
// one stored in PEB. But it's pretty dangerouse because this value could
// be altered in image header.
//

static
int
Nt5(void)
{
	//return NtCuttentTeb()->Peb->OSMajorVersion >= 5;
	return (int)*(int*)((char*)(int)(*(int*)((char*)NtCurrentTeb() + 0x30)) + 0xA4) >= 5;
}




void uuid_generate(uuid_t out)
{
	if(Nt5())
	{
		unsigned char seed[6];
		((NtAllocateUuids_2000)NtAllocateUuids)(out, ((char*)out)+8, ((char*)out)+12, &seed[0] );
	}
	else
	{
		NtAllocateUuids(out, ((char*)out)+8, ((char*)out)+12);
	}
}
