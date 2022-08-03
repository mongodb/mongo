import { initialState } from "./store";

export const showTransitive = (state = initialState, action) => {
  switch (action.type) {
    case "setShowTransitive":
      return action.payload;

    default:
      return state;
  }
};

export const setShowTransitive = (showTransitive) => ({
  type: "setShowTransitive",
  payload: showTransitive,
});
