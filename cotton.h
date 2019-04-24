#include <functional>
#include <cstring>
namespace cotton
{
 int getKey();
 int getSize();
 void async(std::function<void()> &&lambda);
 void start_finish();
 void end_finish();
 void init_runtime();
 void finalize_runtime();
}
