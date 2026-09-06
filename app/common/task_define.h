#ifndef TASK_DEFINE
#define TASK_DEFINE

#define SENSOR_TASK_PRIOR       (5)
#define SENSOR_TASK_STACK_SIZE  (4096 * 2)

#define AUDIO_TASK_PRIOR        (4)
#define AUDIO_TASK_STACK_SIZE   (4096 * 3)   /* +1KB stack cho stdio/fread khi đọc WAV */

#endif /* TASK_DEFINE */