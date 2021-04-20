import { initialState } from "./store";

export const graphPaths = (state = initialState, action) => {
  switch (action.type) {
    case "selectedGraphPaths":
      return action.payload;
    case "setSelectedPath":
      const newState = { ...state, selectedPath: action.payload };
      return newState;
    default:
      return state;
  }
};

export const selectedGraphPaths = (pathData) => ({
  type: "selectedGraphPaths",
  payload: pathData,
});

export const setSelectedPath = (path) => ({
  type: "setSelectedPath",
  payload: path,
});
