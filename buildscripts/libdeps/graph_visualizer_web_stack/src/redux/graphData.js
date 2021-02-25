import { initialState } from "./store";

export const graphData = (state = initialState, action) => {
  switch (action.type) {
    case "setGraphData":
      return action.payload;

    default:
      return state;
  }
};

export const setGraphData = (graphData) => ({
  type: "setGraphData",
  payload: graphData,
});
