#include "yaml-cpp/parser.h"
#include "yaml-cpp/contrib/graphbuilder.h"
#include "graphbuilderadapter.h"

namespace YAML
{
  void *BuildGraphOfNextDocument(Parser& parser, GraphBuilderInterface& graphBuilder)
  {
    GraphBuilderAdapter eventHandler(graphBuilder);
    if (parser.HandleNextDocument(eventHandler)) {
      return eventHandler.RootNode();
    } else {
      return NULL;
    }
  }
}
