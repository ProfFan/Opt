
#include "mLibInclude.h"

using namespace ml;
using Bitmap = ml::ColorImageR8G8B8A8;

extern "C" {
#include "Opt.h"
}


#include <iostream>
#include <string>
#include <vector>
#include <functional>

#include "OptImage.h"
#include "OptGraph.h"

using namespace std;

#include "testFramework.h"