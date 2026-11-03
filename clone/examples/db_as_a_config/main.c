#include <stdint.h>
#include <malloc.h>
#include <memory.h>
#include "../../lib/lib.c"


typedef uint8_t key_t;
typedef char const* cstr_t;


struct size
{
	uint32_t w;
	uint32_t h;
};

struct key_bindings
{
	key_t* Mgz_Len_Field(count) keys;
	uint8_t count;
};

#define Usertag_Len 4
struct game_profile
{
	char usertag[Usertag_Len];
	char const* Mgz_Null_Ended username;

	struct size preferred_resolution;
	uint8_t sound_volume_percentage;
	struct key_bindings key_bindings;
};

struct Mgz_Root_Struct game_settings
{
	struct game_profile* Mgz_Len_Field(count) profiles;
	uint8_t count;
	uint8_t capacity;
};


int32_t main(void)
{
	struct key_bindings kb;
	kb.count = 2;
	kb.keys = malloc(sizeof(key_t) * kb.count);
	kb.keys[0] = 20;
	kb.keys[0] = 21;

	struct game_profile p;
	memcpy(p.usertag, "A123", Usertag_Len);
	p.username = "John";
	p.preferred_resolution = (struct size) { .w = 800, .h = 600 };
	p.sound_volume_percentage = 80;
	p.key_bindings = kb;

	struct game_settings s;
	s.capacity = 10;
	s.count = 0;
	s.profiles = malloc(sizeof(struct game_profile) * s.capacity);

	s.profiles[s.count] = p;
	s.count += 1;

	mgz_flush_to_disk__game_profile(&p);
	return 0;
}
