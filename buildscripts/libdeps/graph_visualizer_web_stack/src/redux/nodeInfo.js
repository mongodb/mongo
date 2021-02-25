import { initialState } from "./store";

export const nodeInfo = (state = initialState, action) => {
  switch (action.type) {
    case "setNodeInfos":
      return action.payload;
    case "addNodeInfo":
      return [...state, action.payload];

    default:
      return state;
  }
};

export const setNodeInfos = (nodeInfos) => ({
  type: "setNodeInfos",
  payload: nodeInfos,
});
